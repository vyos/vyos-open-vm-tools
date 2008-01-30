/*********************************************************
 * Copyright (C) 2007 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/*
 * fileInt.h --
 *
 *	Things internal to the file library.
 */

#if !defined(__FILE_INTERNAL_H__)
#define __FILE_INTERNAL_H__

#define INCLUDE_ALLOW_USERLEVEL
#include "includeCheck.h"
#include "fileLock.h"
#include "unicodeBase.h"

#if defined __linux__
/*
 * These magic constants are used only for parsing Linux statfs data.
 * So they make sense only for Linux build. If you need them on other OSes,
 * think once more.
 */

#define AFFS_SUPER_MAGIC      0xADFF
#define EXT_SUPER_MAGIC       0x137D
#define EXT2_OLD_SUPER_MAGIC  0xEF51
#define EXT2_SUPER_MAGIC      0xEF53
#define HFSPLUS_SUPER_MAGIC   0x482B
#define NFS_SUPER_MAGIC       0x6969
#define SMB_SUPER_MAGIC       0x517B

#if !defined(MSDOS_SUPER_MAGIC)
#define MSDOS_SUPER_MAGIC     0x4D44
#endif

#define XENIX_SUPER_MAGIC     0x012FF7B4
#define SYSV4_SUPER_MAGIC     0x012FF7B5
#define SYSV2_SUPER_MAGIC     0x012FF7B6
#define COH_SUPER_MAGIC       0x012FF7B7
#define UFS_SUPER_MAGIC       0x00011954
#define XFS_SUPER_MAGIC       0x58465342
#define VMFS_SUPER_MAGIC      0x2fABF15E
#define TMPFS_SUPER_MAGIC     0x01021994
#define JFS_SUPER_MAGIC       0x3153464A

#if !defined(REISERFS_SUPER_MAGIC)
#define REISERFS_SUPER_MAGIC  0x52654973
#endif
#endif	// linux

#define LGPFX	"FILE:"

#define FILE_TYPE_REGULAR      0
#define FILE_TYPE_DIRECTORY    1
#define FILE_TYPE_BLOCKDEVICE  2
#define FILE_TYPE_CHARDEVICE   3
#define FILE_TYPE_SYMLINK      4
#define FILE_TYPE_UNCERTAIN    5

typedef struct FileData {
   uint64 fileAccessTime;
   uint64 fileCreationTime;
   uint64 fileModificationTime;
   uint64 fileSize;
   int    fileType;
   int    fileMode;
   int    fileOwner;
   int    fileGroup;
} FileData;

EXTERN int FileAttributes(ConstUnicode pathName,
                          FileData *fileData);

EXTERN int FileRename(ConstUnicode fromPath,
                      ConstUnicode toPath);

EXTERN int FileDeletion(ConstUnicode pathName,
                        Bool handleLink);

EXTERN int FileCreateDirectory(ConstUnicode pathName);
EXTERN int FileRemoveDirectory(ConstUnicode pathName);

#if defined(_WIN32)
EXTERN int FileMapErrorToErrno(char *functionName,
                               DWORD status);
#else
EXTERN char *FilePosixGetBlockDevice(char const *path);
#endif

typedef struct active_lock
{
  struct active_lock *next;
  uint32             age;
  Bool               marked;
  Unicode            dirName;
} ActiveLock;

typedef struct lock_values
{
   char         *machineID;
   char         *executionID;
   char         *payload;
   char         *lockType;
   Unicode      memberName;
   unsigned int lamportNumber;
   uint32       waitTime;
   uint32       msecMaxWaitTime;
   ActiveLock   *lockList;
} LockValues;

#include "file_extensions.h"

#define FILELOCK_SUFFIX "." LOCK_FILE_EXTENSION

#define FILELOCK_DATA_SIZE 512

#if defined(_WIN32)
typedef HANDLE FILELOCK_FILE_HANDLE;
#else
typedef int FILELOCK_FILE_HANDLE;
#endif

EXTERN const char *FileLockGetMachineID(void);

EXTERN Bool FileLockMachineIDMatch(char *host,
                                   char *second);

EXTERN int FileLockMemberValues(ConstUnicode lockDir, 
                                ConstUnicode fileName,
                                char *buffer,
                                uint32 size,
                                LockValues *memberValues);

EXTERN int FileLockHackVMX(const char *machineID,
                           const char *executionID,
                           ConstUnicode filePathName);

EXTERN int FileLockOpenFile(ConstUnicode pathName,
                            int flags,
                            FILELOCK_FILE_HANDLE *handle);

EXTERN int FileLockCloseFile(FILELOCK_FILE_HANDLE handle);

EXTERN int FileLockFileSize(FILELOCK_FILE_HANDLE handle,
                            uint32 *fileSize);

EXTERN int FileLockReadFile(FILELOCK_FILE_HANDLE handle,
                            void *buf,
                            uint32 requestedBytes,
                            uint32 *resultantBytes);

EXTERN int FileLockWriteFile(FILELOCK_FILE_HANDLE handle,
                             void *buf,
                             uint32 requestedBytes,
                             uint32 *resultantBytes);

EXTERN void *FileLockIntrinsic(const char *machineID,
                               const char *executionID,
                               const char *payload,
                               ConstUnicode filePathName,
                               Bool exclusivity,
                               uint32 msecMaxWaitTime,
                               int *err);

EXTERN int FileUnlockIntrinsic(const char *machineID,
                               const char *executionID,
                               ConstUnicode filePathName,
                               const void *lockToken);

EXTERN Bool FileLockValidOwner(const char *executionID,
                               const char *payload);

EXTERN Bool FileLockValidName(ConstUnicode fileName);

#endif
