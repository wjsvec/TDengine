/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"
#include "taos.h"
#include "tnote.h"
#include "ttimer.h"
#include "tconfig.h"
#include "tfile.h"
#include "twal.h"
#include "tfs.h"
#include "tsync.h"
#include "dnodeStep.h"
#include "dnodePeer.h"
#include "dnodeModule.h"
#include "dnodeEps.h"
#include "dnodeMInfos.h"
#include "dnodeCfg.h"
#include "dnodeCheck.h"
#include "dnodeVRead.h"
#include "dnodeVWrite.h"
#include "dnodeVMgmt.h"
#include "dnodeVnodes.h"
#include "dnodeMRead.h"
#include "dnodeMWrite.h"
#include "dnodeMPeer.h"
#include "dnodeShell.h"
#include "dnodeTelemetry.h"
#include "module.h"
#include "mnode.h"
#include "qScript.h"
#include "tcache.h"
#include "tscompression.h"

#if !defined(_MODULE) || !defined(_TD_LINUX)
int32_t moduleStart() { return 0; }
void    moduleStop() {}
#endif


void *tsDnodeTmr = NULL;
static SRunStatus tsRunStatus = TSDB_RUN_STATUS_STOPPED;

static int32_t dnodeInitStorage();
static void    dnodeCleanupStorage();
static void    dnodeSetRunStatus(SRunStatus status);
static void    dnodeCheckDataDirOpenned(char *dir);
static int     dnodeCreateDir(const char *dir);

static SStep tsDnodeSteps[] = {
  {"dnode-tfile",     tfInit,              tfCleanup},
  {"dnode-rpc",       rpcInit,             rpcCleanup},
  {"dnode-globalcfg", taosCheckGlobalCfg,  NULL},
  {"dnode-storage",   dnodeInitStorage,    dnodeCleanupStorage},
  {"dnode-cfg",       dnodeInitCfg,        dnodeCleanupCfg},
  {"dnode-eps",       dnodeInitEps,        dnodeCleanupEps},
  {"dnode-minfos",    dnodeInitMInfos,     dnodeCleanupMInfos},
  {"dnode-wal",       walInit,             walCleanUp},
  {"dnode-sync",      syncInit,            syncCleanUp},
  {"dnode-check",     dnodeInitCheck,      dnodeCleanupCheck},     // NOTES: dnodeInitCheck must be behind the dnodeinitStorage component !!!
  {"dnode-vread",     dnodeInitVRead,      dnodeCleanupVRead},
  {"dnode-vwrite",    dnodeInitVWrite,     dnodeCleanupVWrite},
  {"dnode-vmgmt",     dnodeInitVMgmt,      dnodeCleanupVMgmt},
  {"dnode-mread",     dnodeInitMRead,      NULL},
  {"dnode-mwrite",    dnodeInitMWrite,     NULL},
  {"dnode-mpeer",     dnodeInitMPeer,      NULL},  
  {"dnode-client",    dnodeInitClient,     dnodeCleanupClient},
  {"dnode-server",    dnodeInitServer,     dnodeCleanupServer},
  {"dnode-vnodes",    dnodeInitVnodes,     dnodeCleanupVnodes},
  {"dnode-modules",   dnodeInitModules,    dnodeCleanupModules},
  {"dnode-mread",     NULL,                dnodeCleanupMRead},
  {"dnode-mwrite",    NULL,                dnodeCleanupMWrite},
  {"dnode-mpeer",     NULL,                dnodeCleanupMPeer},  
  {"dnode-shell",     dnodeInitShell,      dnodeCleanupShell},
  {"dnode-statustmr", dnodeInitStatusTimer,dnodeCleanupStatusTimer},
  {"dnode-telemetry", dnodeInitTelemetry,  dnodeCleanupTelemetry},
  {"dnode-script",    scriptEnvPoolInit,   scriptEnvPoolCleanup},
};

static SStep tsDnodeCompactSteps[] = {
  {"dnode-tfile",     tfInit,              tfCleanup},
  {"dnode-globalcfg", taosCheckGlobalCfg,  NULL},
  {"dnode-storage",   dnodeInitStorage,    dnodeCleanupStorage},
  {"dnode-cfg",       dnodeInitCfg,        dnodeCleanupCfg},
  {"dnode-eps",       dnodeInitEps,        dnodeCleanupEps},
  {"dnode-minfos",    dnodeInitMInfos,     dnodeCleanupMInfos},
  {"dnode-wal",       walInit,             walCleanUp},
  {"dnode-sync",      syncInit,            syncCleanUp},
  {"dnode-vread",     dnodeInitVRead,      dnodeCleanupVRead},
  {"dnode-vwrite",    dnodeInitVWrite,     dnodeCleanupVWrite},
  {"dnode-vmgmt",     dnodeInitVMgmt,      dnodeCleanupVMgmt},
  {"dnode-mread",     dnodeInitMRead,      NULL},
  {"dnode-mwrite",    dnodeInitMWrite,     NULL},
  {"dnode-mpeer",     dnodeInitMPeer,      NULL},
  {"dnode-vnodes",    dnodeInitVnodes,     dnodeCleanupVnodes},
  {"dnode-modules",   dnodeInitModules,    dnodeCleanupModules},
  {"dnode-mread",     NULL,                dnodeCleanupMRead},
  {"dnode-mwrite",    NULL,                dnodeCleanupMWrite},
  {"dnode-mpeer",     NULL,                dnodeCleanupMPeer},
};

static int dnodeCreateDir(const char *dir) {
  if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
    return -1;
  }
  
  return 0;
}

static void dnodeCleanupComponents() {
  if (!tsCompactMnodeWal) {
    int32_t stepSize = sizeof(tsDnodeSteps) / sizeof(SStep);
    dnodeStepCleanup(tsDnodeSteps, stepSize);
  } else {
    int32_t stepSize = sizeof(tsDnodeCompactSteps) / sizeof(SStep);
    dnodeStepCleanup(tsDnodeCompactSteps, stepSize);
  }
}

static int32_t dnodeInitComponents() {
  if (!tsCompactMnodeWal) {
    int32_t stepSize = sizeof(tsDnodeSteps) / sizeof(SStep);
    return dnodeStepInit(tsDnodeSteps, stepSize);
  } else {
    int32_t stepSize = sizeof(tsDnodeCompactSteps) / sizeof(SStep);
    return dnodeStepInit(tsDnodeCompactSteps, stepSize);
  }
}

static int32_t dnodeInitTmr() {
  tsDnodeTmr = taosTmrInit(100, 200, 60000, "DND-DM");
  if (tsDnodeTmr == NULL) {
    dError("failed to init dnode timer");
    return -1;
  }

  return 0;
}

static void dnodeCleanupTmr() {
  if (tsDnodeTmr != NULL) {
    taosTmrCleanUp(tsDnodeTmr);
    tsDnodeTmr = NULL;
  }
}

int32_t dnodeInitSystem() {
  dnodeSetRunStatus(TSDB_RUN_STATUS_INITIALIZE);
  tscEmbedded  = 1;
  taosIgnSIGPIPE();
  taosBlockSIGPIPE();
  taosResolveCRC();
  taosInitGlobalCfg();
  taosReadGlobalLogCfg();
  taosSetCoreDump();
  dnodeInitTmr();

  if (dnodeCreateDir(tsLogDir) < 0) {
   printf("failed to create dir: %s, reason: %s\n", tsLogDir, strerror(errno));
   return -1;
  }

  char temp[TSDB_FILENAME_LEN];
  sprintf(temp, "%s/taosdlog", tsLogDir);
  if (taosInitLog(temp, tsNumOfLogLines, 1) < 0) {
    printf("failed to init log file\n");
  }

  if (!taosReadGlobalCfg()) {
    taosPrintGlobalCfg();
    dError("TDengine read global config failed");
    return -1;
  }

  dInfo("start to initialize TDengine");

  taosInitNotes();

  if (dnodeInitComponents() != 0) {
    return -1;
  }

  dnodeSetRunStatus(TSDB_RUN_STATUS_RUNING);
  moduleStart();

  tsDnodeStartTime = taosGetTimestampMs();
  dnodeReportStep("TDengine", "initialized successfully", 1);
  dInfo("TDengine is initialized successfully");

  return 0;
}

void dnodeCleanUpSystem() {
  if (dnodeGetRunStatus() != TSDB_RUN_STATUS_STOPPED) {
    moduleStop();
    dnodeSetRunStatus(TSDB_RUN_STATUS_STOPPED);
    dnodeCleanupTmr();
    dnodeCleanupComponents();
    taos_cleanup();
    taosCloseLog();
    taosStopCacheRefreshWorker();
  }
}

SRunStatus dnodeGetRunStatus() {
  return tsRunStatus;
}

static void dnodeSetRunStatus(SRunStatus status) {
  tsRunStatus = status;
}

static void dnodeCheckDataDirOpenned(char *dir) {
  char filepath[256] = {0};
  sprintf(filepath, "%s/.running", dir);

  int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU | S_IRWXG | S_IRWXO);
  if (fd < 0) {
    dError("failed to open lock file:%s, reason: %s, quit", filepath, strerror(errno));
    exit(0);
  }
  int32_t ret = flock(fd, LOCK_EX | LOCK_NB);
  if (ret != 0) {
    dError("failed to lock file:%s ret:%d[%s], database may be running, quit", filepath, ret, strerror(errno));
    close(fd);
    exit(0);
  }
}

static int32_t dnodeInitStorage() {
#ifdef TD_TSZ
  // compress module init
  tsCompressInit();
#endif

  // storage module init
  if (tsDiskCfgNum == 1 && dnodeCreateDir(tsDataDir) < 0) {
    dError("failed to create dir: %s, reason: %s", tsDataDir, strerror(errno));
    return -1;
  } 
  
  if (tfsInit(tsDiskCfg, tsDiskCfgNum) < 0) {
    dError("failed to init TFS since %s", tstrerror(terrno));
    return -1;
  }
  strncpy(tsDataDir, TFS_PRIMARY_PATH(), TSDB_FILENAME_LEN);
  sprintf(tsMnodeDir, "%s/mnode", tsDataDir);
  sprintf(tsVnodeDir, "%s/vnode", tsDataDir);
  sprintf(tsDnodeDir, "%s/dnode", tsDataDir);
  // sprintf(tsVnodeBakDir, "%s/vnode_bak", tsDataDir);

  if (tsCompactMnodeWal == 1) {
    sprintf(tsMnodeTmpDir, "%s/mnode_tmp", tsDataDir);
    if (taosDirExist(tsMnodeTmpDir)) {
      dError("mnode_tmp dir already exist in %s,quit compact job", tsMnodeTmpDir);
      return -1;
    }
    if (dnodeCreateDir(tsMnodeTmpDir) < 0) {
      dError("failed to create dir: %s, reason: %s", tsMnodeTmpDir, strerror(errno));
      return -1;
    }

    sprintf(tsMnodeBakDir, "%s/mnode_bak", tsDataDir);
    if (taosDirExist(tsMnodeBakDir)) {
      dError("mnode_bak dir already exist in %s,quit compact job", tsMnodeBakDir);
      return -1;
    }
  }
  //TODO(dengyihao): no need to init here
  if (dnodeCreateDir(tsMnodeDir) < 0) {
   dError("failed to create dir: %s, reason: %s", tsMnodeDir, strerror(errno));
   return -1;
  } 

  if (dnodeCreateDir(tsDnodeDir) < 0) {
   dError("failed to create dir: %s, reason: %s", tsDnodeDir, strerror(errno));
   return -1;
  }

  if (tfsMkdir("vnode") < 0) {
    dError("failed to create vnode dir since %s", tstrerror(terrno));
    return -1;
  }

  if (tfsMkdir("vnode_bak") < 0) {
    dError("failed to create vnode_bak dir since %s", tstrerror(terrno));
    return -1;
  }

  TDIR *tdir = tfsOpendir("vnode_bak/.staging");
  bool stagingNotEmpty = tfsReaddir(tdir) != NULL;
  tfsClosedir(tdir);

  if (stagingNotEmpty) {
    dError("vnode_bak/.staging dir not empty, fix it first.");
    return -1;
  }

  if (tfsMkdir("vnode_bak/.staging") < 0) {
    dError("failed to create vnode_bak/.staging dir since %s", tstrerror(terrno));
    return -1;
  }

  dnodeCheckDataDirOpenned(tsDnodeDir);

  taosGetDisk();
  taosPrintDiskInfo();
  dInfo("dnode storage is initialized at %s", tsDnodeDir);
  return 0;
}

static void dnodeCleanupStorage() {
  // storage destroy
  tfsDestroy();

 #ifdef TD_TSZ
  // compress destroy
  tsCompressExit();
 #endif
}

bool  dnodeIsFirstDeploy() {
  return strcmp(tsFirst, tsLocalEp) == 0;
}
