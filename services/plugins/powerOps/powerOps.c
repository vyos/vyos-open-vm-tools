/*********************************************************
 * Copyright (C) 2008 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/**
 * @file powerOps.c
 *
 * Plugin that handles power operation events from the VMX.
 */

#define G_LOG_DOMAIN "powerops"

#include "vm_assert.h"
#include "vm_basic_defs.h"
#include "vmtoolsApp.h"

#include "conf.h"
#include "procMgr.h"
#include "system.h"
#include "vm_app.h"
#include "vmtools.h"

#if defined(G_PLATFORM_WIN32)
#  define INVALID_PID NULL
#else
#  define INVALID_PID (GPid) -1
#endif

#if !defined(__APPLE__)
#include "embed_version.h"
#include "vmtoolsd_version.h"
VM_EMBED_VERSION(VMTOOLSD_VERSION_STRING);
#endif

static const char *stateChgConfNames[] = {
   NULL,
   CONFNAME_POWEROFFSCRIPT,
   CONFNAME_POWEROFFSCRIPT,
   CONFNAME_POWERONSCRIPT,
   CONFNAME_RESUMESCRIPT,
   CONFNAME_SUSPENDSCRIPT,
};


/** Internal plugin state. */
typedef struct PowerOpState {
   GuestOsState         stateChgInProgress;
   GuestOsState         lastFailedStateChg;
#if defined(G_PLATFORM_WIN32)
   ProcMgr_AsyncProc   *pid;
#else
   GPid                 pid;
#endif
   ToolsAppCtx         *ctx;
} PowerOpState;


/**
 * Returns the capabilities of the power ops plugin.
 *
 * @param[in]  src      The source object.
 * @param[in]  ctx      The application context.
 * @param[in]  set      Whether capabilities are being set.
 * @param[in]  data     Unused.
 *
 * @return List of capabilities.
 */

static GArray *
PowerOpsCapabilityCb(gpointer src,
                     ToolsAppCtx *ctx,
                     gboolean set,
                     gpointer data)
{
   const ToolsAppCapability caps[] = {
      { TOOLS_CAP_OLD_NOVAL, "statechange", 0, 1 },
      { TOOLS_CAP_OLD_NOVAL, "softpowerop_retry", 0, 1 },
   };

   return VMTools_WrapArray(caps, sizeof *caps, ARRAYSIZE(caps));
}


/**
 * Clean up internal state on shutdown.
 *
 * @param[in]  src      The source object.
 * @param[in]  ctx      Unused.
 * @param[in]  plugin   Plugin registration data.
 */

static void
PowerOpsShutdown(gpointer src,
                 ToolsAppCtx *ctx,
                 ToolsPluginData *plugin)
{
   PowerOpState *state = plugin->_private;
   g_free(state);
}


/**
 * Called when a state change script is done running. Sends the state change
 * status to the VMX.
 *
 * @note This may halt/reboot the VM. Also the VMX may suspend the VM upon
 * receipt of a positive status.
 *
 * @param[in]  state       Plugin state.
 * @param[in]  success     Whether the script was successful.
 */

static void
PowerOpsStateChangeDone(PowerOpState *state,
                        gboolean success)
{
   gchar *msg;

   g_debug("State change complete, success = %d.\n", success);

   /*
    * We execute the requested action if the script succeeded, or if the
    * same action was tried before but didn't finish due to a script failure.
    * See bug 168568 for discussion.
    */
   if (success || state->lastFailedStateChg == state->stateChgInProgress) {
      success = TRUE;
      state->lastFailedStateChg = GUESTOS_STATECHANGE_NONE;
   }

   if (!success) {
      state->lastFailedStateChg = state->stateChgInProgress;
   }

   /* Send the status message to the VMX. */
   msg = g_strdup_printf("tools.os.statechange.status %d %d",
                         success,
                         state->stateChgInProgress);
   if (!RpcChannel_Send(state->ctx->rpc, msg, strlen(msg) + 1, NULL, NULL)) {
      g_warning("Unable to send the status RPC.");
   }

   g_free(msg);

   /* Finally, perform the requested operation. */
   if (success) {
      if (state->stateChgInProgress == GUESTOS_STATECHANGE_REBOOT) {
         g_debug("Initiating reboot.\n");
         System_Shutdown(TRUE);
      } else if (state->stateChgInProgress == GUESTOS_STATECHANGE_HALT) {
         g_debug("Initiating halt.\n");
         System_Shutdown(FALSE);
      }
   }

   state->stateChgInProgress = GUESTOS_STATECHANGE_NONE;
}


#if defined(G_PLATFORM_WIN32)
/**
 * Callback for when the script process finishes on Win32 systems.
 *
 * @param[in]  _state      Plugin state.
 *
 * @return TRUE if the process is not finished yet.
 */

static gboolean
PowerOpsScriptCallback(gpointer _state)
{
   PowerOpState *state = _state;

   g_assert(state->pid != INVALID_PID);

   if (!ProcMgr_IsAsyncProcRunning(state->pid)) {
      int exitcode;
      gboolean success;

      success = (ProcMgr_GetExitCode(state->pid, &exitcode) == 0 &&
                 exitcode == 0);
      g_debug("Script exit code: %d, success = %d\n", exitcode, success);
      PowerOpsStateChangeDone(state, success);
      ProcMgr_Free(state->pid);
      state->pid = INVALID_PID;
      return FALSE;
   }
   return TRUE;
}


/**
 * Starts a process using the ProcMgr library. For some reason the glib
 * spawn functions are a pain to use under Windows, and ProcMgr works
 * fine since we can use the "selectable" handle to monitor the child
 * process.
 *
 * XXX: as soon as I figure out what's wrong with the "gspawn-win32-helper"
 * process and why it's not working, this should probably be merged with the
 * POSIX code below.
 *
 * @param[in]  state    Plugin state.
 * @param[in]  script   Path to the script to be run.
 *
 * @return Whether started the process successfully.
 */

static gboolean
PowerOpsRunScript(PowerOpState *state,
                  gchar *script)
{
   gchar *quoted = NULL;
   ProcMgr_ProcArgs procArgs;

   /*
    * Pass the CREATE_NO_WINDOW flag to CreateProcess so that the
    * cmd.exe window will not be visible to the user in the guest.
    */
   memset(&procArgs, 0, sizeof procArgs);
   procArgs.bInheritHandles = TRUE;
   procArgs.dwCreationFlags = CREATE_NO_WINDOW;

   /* Quote the path if it's not yet quoted. */
   if (script[0] != '"') {
      quoted = g_strdup_printf("\"%s\"", script);
   }

   g_debug("Executing script: %s\n", (quoted != NULL) ? quoted : script);
   state->pid = ProcMgr_ExecAsync((quoted != NULL) ? quoted : script, &procArgs);
   g_free(quoted);

   if (state->pid != NULL) {
      HANDLE h = ProcMgr_GetAsyncProcSelectable(state->pid);
      GSource *watch = VMTools_NewHandleSource(h);
      VMTOOLSAPP_ATTACH_SOURCE(state->ctx, watch, PowerOpsScriptCallback, state, NULL);
      g_source_unref(watch);
      return TRUE;
   } else {
      g_warning("Failed to start script: out of memory?\n");
      return FALSE;
   }
}

#else

/**
 * Callback for when the script process finishes on POSIX systems.
 *
 * @param[in]  pid         Child pid.
 * @param[in]  status      Exit status of script.
 * @param[in]  _state      Plugin state.
 *
 * @return FALSE.
 */

static gboolean
PowerOpsScriptCallback(GPid pid,
                       gint status,
                       gpointer _state)
{
   PowerOpState *state = _state;

   g_assert(state->pid != INVALID_PID);

   PowerOpsStateChangeDone(_state, status == 0);
   g_spawn_close_pid(state->pid);
   state->pid = INVALID_PID;
   return FALSE;
}


/**
 * Starts a process using glib. This works better in the non-Windows case,
 * since glib provides a nice API for monitoring when a process exits
 * (instead of having to poll the process every once in a while when using
 * ProcMgr.)
 *
 * @param[in]  state    Plugin state.
 * @param[in]  script   Path to the script to be run.
 *
 * @return Whether started the process successfully.
 */

static gboolean
PowerOpsRunScript(PowerOpState *state,
                  gchar *script)
{
   gchar *argv[2];
   GSource *watch;
   GError *err = NULL;

   argv[0] = g_filename_from_utf8(script, -1, NULL, NULL, &err);
   if (err != NULL) {
      g_error("Conversion error: %s\n", err->message);
   }
   argv[1] = NULL;

   if (!g_spawn_async(NULL,
                      argv,
                      NULL,
                      G_SPAWN_DO_NOT_REAP_CHILD |
                      G_SPAWN_STDOUT_TO_DEV_NULL |
                      G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL,
                      NULL,
                      &state->pid,
                      &err)) {
         g_warning("Error starting script: %s\n", err->message);
         g_clear_error(&err);
         g_free(argv[0]);
         return FALSE;
   }

   /* Setup a watch for when the child is done. */
   watch = g_child_watch_source_new(state->pid);
   VMTOOLSAPP_ATTACH_SOURCE(state->ctx, watch, PowerOpsScriptCallback, state, NULL);
   g_source_unref(watch);
   g_free(argv[0]);
   return TRUE;
}
#endif


/**
 * Handler for commands which invoke state change scripts. Runs the configured
 * script for the power operation signaled by the host.
 *
 * @param[in]  data     RPC data.
 *
 * @return TRUE on success.
 */

static Bool
PowerOpsStateChange(RpcInData *data)
{
   size_t i;
   PowerOpState *state = data->clientData;

   if (state->pid != INVALID_PID) {
      g_debug("State change already in progress.\n");
      return RPCIN_SETRETVALS(data,  "State change already in progress", FALSE);
   }

   g_debug("State change: %s\n", data->name);

   for (i = 0; i < ARRAYSIZE(stateChangeCmdTable); i++) {
      if (strcmp(data->name, stateChangeCmdTable[i].tcloCmd) == 0) {
         gchar *script;
         const char *result;
         Bool ret;

         state->stateChgInProgress = stateChangeCmdTable[i].id;

         script = g_key_file_get_string(state->ctx->config,
                                        "powerops",
                                        stateChgConfNames[stateChangeCmdTable[i].id],
                                        NULL);

         if (script == NULL) {
            /* Use default script if not set in config file. */
            char *dfltPath;
            const char *dfltScript;

            dfltScript = GuestApp_GetDefaultScript(stateChangeCmdTable[i].name);
            if (dfltScript == NULL) {
               g_debug("No default script to run for state change %s.\n",
                       stateChangeCmdTable[i].name);
               PowerOpsStateChangeDone(state, TRUE);
               return RPCIN_SETRETVALS(data, "", TRUE);
            }
            dfltPath = GuestApp_GetInstallPath();
            g_assert(dfltPath != NULL);
            script = g_strdup_printf("%s%c%s", dfltPath, DIRSEPC, dfltScript);
            vm_free(dfltPath);
         } else if (strlen(script) == 0) {
            g_debug("No script to run for state change %s.\n",
                    stateChangeCmdTable[i].name);
            g_free(script);
            PowerOpsStateChangeDone(state, TRUE);
            return RPCIN_SETRETVALS(data, "", TRUE);
         }

         if (PowerOpsRunScript(state, script)) {
            result = "";
            ret = TRUE;
         } else {
            PowerOpsStateChangeDone(state, FALSE);
            result = "Error starting script";
            ret = FALSE;
         }

         g_free(script);
         return RPCIN_SETRETVALS(data, result, ret);
      }
   }

   g_warning("Invalid state change command.\n");
   return RPCIN_SETRETVALS(data, "Invalid state change command", FALSE);
}


/**
 * Plugin entry point. Returns the registration data.
 *
 * @param[in]  ctx   Unused.
 *
 * @return The registration data.
 */

TOOLS_MODULE_EXPORT ToolsPluginData *
ToolsOnLoad(ToolsAppCtx *ctx)
{
   static ToolsPluginData regData = {
      "powerops",
      NULL,
      NULL
   };

   size_t i;
   PowerOpState *state;

   ToolsPluginSignalCb sigs[] = {
      { TOOLS_CORE_SIG_CAPABILITIES, PowerOpsCapabilityCb, NULL },
      { TOOLS_CORE_SIG_SHUTDOWN, PowerOpsShutdown, &regData }
   };
   ToolsAppReg regs[] = {
      { TOOLS_APP_GUESTRPC, NULL },
      { TOOLS_APP_SIGNALS, VMTools_WrapArray(sigs, sizeof *sigs, ARRAYSIZE(sigs)) }
   };

   state = g_malloc0(sizeof *state);
   state->ctx = ctx;
   state->pid = INVALID_PID;

   regs[0].data = g_array_sized_new(FALSE,
                                    TRUE,
                                    sizeof (RpcChannelCallback),
                                    ARRAYSIZE(stateChangeCmdTable));

   for (i = 0; i < ARRAYSIZE(stateChangeCmdTable); i++) {
      RpcChannelCallback *cb = &g_array_index(regs[0].data, RpcChannelCallback, i);
      cb->name = stateChangeCmdTable[i].tcloCmd;
      cb->callback = PowerOpsStateChange;
      cb->clientData = state;
   }

   regData.regs = VMTools_WrapArray(regs, sizeof *regs, ARRAYSIZE(regs));
   regData._private = state;
   return &regData;
}

