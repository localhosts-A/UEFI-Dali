##
# Copyright (c) Microsoft Corporation.
# SPDX-License-Identifier: BSD-2-Clause-Patent
##

import datetime
import logging
import os
import re
import uuid
import importlib.util

from io import StringIO
from pathlib import Path

from edk2toolext.environment import shell_environment
from edk2toolext.environment.uefi_build import UefiBuilder
from edk2toolext.invocables.edk2_platform_build import BuildSettingsManager
from edk2toolext.invocables.edk2_pr_eval import PrEvalSettingsManager
from edk2toolext.invocables.edk2_setup import (RequiredSubmodule, SetupSettingsManager)
from edk2toolext.invocables.edk2_update import UpdateSettingsManager
from edk2toolext.invocables.edk2_parse import ParseSettingsManager
from edk2toollib.utility_functions import RunCmd

# ####################################################################################### #
#                                Common Configuration                                     #
# ####################################################################################### #
class CommonPlatform ():
    PackagesSupported = ("daliPkg",)
    ArchSupported = ("AARCH64")
    TargetsSupported = ("DEBUG", "RELEASE")
    Scopes = ('dali', 'gcc_aarch64_linux', 'edk2-build')
    WorkspaceRoot = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    PackagesPath = (
        "Platforms/Xiaomi",
        os.environ.get("DALI_UNIXNU_PACKAGE", os.path.join(WorkspaceRoot, "../UniXNU/package")),
        os.environ.get("DALI_DARWINBOOT_PACKAGE", os.path.join(WorkspaceRoot, "../UniXNU/drivers/DarwinBoot")),
        "Common/Mu",
        "Common/Mu_OEM_Sample",
        "Mu_Basecore",
        "Silicon/MediaTek",
        "Silicon/Silicium",
        "Silicium-ACPI",
        "Silicium-ACPI/Platforms/Xiaomi",
        "Silicium-ACPI/Silicon/MediaTek"
    )

# ####################################################################################### #
#                         Configuration for Update & Setup                                #
# ####################################################################################### #
class SettingsManager (UpdateSettingsManager, SetupSettingsManager, PrEvalSettingsManager, ParseSettingsManager):

    def GetPackagesSupported (self):
        return CommonPlatform.PackagesSupported

    def GetArchitecturesSupported (self):
        return CommonPlatform.ArchSupported

    def GetTargetsSupported (self):
        return CommonPlatform.TargetsSupported

    def GetRequiredSubmodules (self):
        return [
            RequiredSubmodule ("Common/Mu", False),
            RequiredSubmodule ("Common/Mu_OEM_Sample", False),
            RequiredSubmodule ("Mu_Basecore", False),
            RequiredSubmodule ("Silicium-ACPI", False),
            RequiredSubmodule ("Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl", False)
        ]

    def SetArchitectures (self, list_of_requested_architectures):
        unsupported = set(list_of_requested_architectures) - set(self.GetArchitecturesSupported())

        if (len(unsupported) > 0):
            errorString = ("Unsupported Architecture Requested: " + " ".join(unsupported))
            logging.critical (errorString)
            raise Exception (errorString)

        self.ActualArchitectures = list_of_requested_architectures

    def GetWorkspaceRoot (self):
        return CommonPlatform.WorkspaceRoot

    def GetActiveScopes (self):
        return CommonPlatform.Scopes

    def FilterPackagesToTest (self, changedFilesList: list, potentialPackagesList: list) -> list:
        build_these_packages = []
        possible_packages = potentialPackagesList.copy ()

        for f in changedFilesList:
            if "BaseTools" in f:
                if os.path.splitext(f) not in [".txt", ".md"]:
                    build_these_packages = possible_packages
                    break

            if "platform-build-run-steps.yml" in f:
                build_these_packages = possible_packages
                break

        return build_these_packages

    def GetPlatformDscAndConfig (self) -> tuple:
        return ("daliPkg/dali.dsc", {})

    def GetName (self):
        return "dali"

    def GetPackagesPath (self):
        return CommonPlatform.PackagesPath

# ####################################################################################### #
#                         Actual Configuration for Platform Build                         #
# ####################################################################################### #
class PlatformBuilder (UefiBuilder, BuildSettingsManager):
    def __init__ (self):
        UefiBuilder.__init__ (self)

    def AddCommandLineOptions (self, parserObj):
        parserObj.add_argument('-a', "--arch", dest="build_arch", type=str, default="AARCH64", help="Optional - CSV of architecture to build. AARCH64 is used for PEI and DXE and is the only valid option for this platform.")

    def RetrieveCommandLineOptions (self, args):
        if args.build_arch.upper() != "AARCH64":
            raise Exception("Invalid Arch Specified.  Please see comments in DeviceBuild.py::PlatformBuilder::AddCommandLineOptions")

    def GetWorkspaceRoot (self):
        return CommonPlatform.WorkspaceRoot

    def GetPackagesPath (self):
        result = [ shell_environment.GetBuildVars().GetValue("FEATURE_CONFIG_PATH", "") ]

        for a in CommonPlatform.PackagesPath:
            result.append(a)

        return result

    def GetActiveScopes (self):
        return CommonPlatform.Scopes

    def GetName (self):
        return "daliPkg"

    def GetLoggingLevel (self, loggerType):
        return logging.INFO
        return super().GetLoggingLevel(loggerType)

    def SetPlatformEnv (self):
        logging.debug ("PlatformBuilder SetPlatformEnv")

        self.env.SetValue ("PRODUCT_NAME", "dali", "Platform Hardcoded")
        self.env.SetValue ("ACTIVE_PLATFORM", "daliPkg/dali.dsc", "Platform Hardcoded")
        self.env.SetValue ("TARGET_ARCH", "AARCH64", "Platform Hardcoded")
        self.env.SetValue ("TOOL_CHAIN_TAG", "CLANGPDB", "set default to clangpdb")
        self.env.SetValue ("EMPTY_DRIVE", "FALSE", "Default to false")
        self.env.SetValue ("RUN_TESTS", "FALSE", "Default to false")
        self.env.SetValue ("SHUTDOWN_AFTER_RUN", "FALSE", "Default to false")
        self.env.SetValue ("BLD_*_BUILDID_STRING", "Unknown", "Default")
        self.env.SetValue ("BUILDREPORTING", "TRUE", "Enabling build report")
        self.env.SetValue ("BUILDREPORT_TYPES", "PCD DEPEX FLASH BUILD_FLAGS LIBRARY FIXED_ADDRESS HASH", "Setting build report types")
        self.env.SetValue ("BLD_*_MEMORY_PROTECTION", "TRUE", "Default")
        self.env.SetValue ("BLD_*_SHIP_MODE", "FALSE", "Default")
        self.env.SetValue ("BLD_*_ENABLE_SECUREBOOT", self.env.GetValue("ENABLE_SECUREBOOT"), "Default")
        self.env.SetValue ("BLD_*_FD_BASE", self.env.GetValue("FD_BASE"), "Default")
        self.env.SetValue ("BLD_*_FD_SIZE", self.env.GetValue("FD_SIZE"), "Default")
        self.env.SetValue ("BLD_*_FD_BLOCKS", self.env.GetValue("FD_BLOCKS"), "Default")
        self.env.SetValue ("BLD_*_DALI_PAYLOAD_IMAGE", os.environ.get("DALI_PAYLOAD_IMAGE", os.path.join(CommonPlatform.WorkspaceRoot, "out/payload.img")), "Dali RAM FAT")
        ufs_probe = os.environ.get("DALI_UFS_PROBE", "false")
        if ufs_probe not in ("true", "false"):
            raise ValueError("DALI_UFS_PROBE must be true or false")
        ufs_token = os.environ.get("DALI_RUN_TOKEN", "none") if ufs_probe == "true" else "none"
        if ufs_probe == "true" and (not re.fullmatch(r"[0-9a-f]{32}", ufs_token) or
                                    os.environ.get("DALI_AUTO_FASTBOOT") != "false"):
            raise ValueError("UFS probe requires a fresh 32-hex DALI_RUN_TOKEN and DALI_AUTO_FASTBOOT=false")
        self.env.SetValue ("BLD_*_DALI_UFS_PROBE", "TRUE" if ufs_probe == "true" else "FALSE", "Opt-in UFS snapshot")
        self.env.SetValue ("BLD_*_DALI_UFS_PROBE_TOKEN", '"' + ufs_token + '"', "Bound UFS snapshot token")

        ufs_read_only = os.environ.get("DALI_UFS_READ_ONLY", "false")
        if ufs_read_only not in ("true", "false"):
            raise ValueError("DALI_UFS_READ_ONLY must be true or false")
        storage_token = os.environ.get("DALI_RUN_TOKEN", "none") if ufs_read_only == "true" else "none"
        if ufs_read_only == "true" and (ufs_probe != "false" or
                not re.fullmatch(r"[0-9a-f]{32}", storage_token) or
                os.environ.get("DALI_AUTO_FASTBOOT") != "false" or os.environ.get("DALI_RESET_METHOD")):
            raise ValueError("UFS host diagnostic requires its own token and no competing probe/reset mode")
        self.env.SetValue ("BLD_*_DALI_UFS_READ_ONLY", "TRUE" if ufs_read_only == "true" else "FALSE", "Opt-in physical UFS diagnostic")
        self.env.SetValue ("BLD_*_DALI_UFS_STORAGE_TOKEN", '"' + storage_token + '"', "Bound physical UFS diagnostic token")

        log_tool = Path(CommonPlatform.WorkspaceRoot) / "tools/dali/ufs_log_settings.py"
        log_spec = importlib.util.spec_from_file_location("dali_ufs_log_settings", log_tool)
        log_module = importlib.util.module_from_spec(log_spec)
        log_spec.loader.exec_module(log_module)
        log_enabled, log_wire = log_module.log_settings(os.environ)
        self.env.SetValue ("BLD_*_DALI_UFS_LOG_ENABLED", "TRUE" if log_enabled else "FALSE", "Explicit manual userdata log diagnostic")
        self.env.SetValue ("BLD_*_DALI_UFS_SCRATCH_ENABLED", "TRUE" if log_enabled else "FALSE", "Measured userdata log proposal")
        self.env.SetValue ("BLD_*_DALI_UFS_SCRATCH_WIRE", '"' + log_wire + '"', "Target-bound log wire; runtime observations still required")

        source_tool = Path(CommonPlatform.WorkspaceRoot) / "tools/dali/ufs_source_record.py"
        spec = importlib.util.spec_from_file_location("dali_ufs_source_record", source_tool)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        measure, record = module.measurement_settings(os.environ, CommonPlatform.WorkspaceRoot)
        self.env.SetValue ("BLD_*_DALI_UFS_MEASURE_PAYLOAD", "TRUE" if measure else "FALSE", "RAM kernel provenance observation")
        # Stuart passes -D through a shell. Protect the byte-array braces from
        # brace expansion into many positional build targets.
        self.env.SetValue ("BLD_*_DALI_UFS_SOURCE_RECORD", '"' + record + '"', "Compiled source revisions from local build inputs")

        return 0

    def PlatformPreBuild (self):
        return 0

    def PlatformPostBuild (self):
        return 0

    def FlashRomImage (self):
        return 0

if __name__ == "__main__":
    import argparse
    import sys

    from edk2toolext.invocables.edk2_platform_build import Edk2PlatformBuild
    from edk2toolext.invocables.edk2_setup import Edk2PlatformSetup
    from edk2toolext.invocables.edk2_update import Edk2Update

    SCRIPT_PATH = os.path.relpath (__file__)

    parser = argparse.ArgumentParser (add_help=False)

    parse_group = parser.add_mutually_exclusive_group()

    parse_group.add_argument ("--update", "--UPDATE", action='store_true', help="Invokes stuart_update")
    parse_group.add_argument ("--setup",  "--SETUP",  action='store_true', help="Invokes stuart_setup")

    args, remaining = parser.parse_known_args()

    new_args = ["stuart", "-c", SCRIPT_PATH]
    new_args = new_args + remaining

    sys.argv = new_args

    if args.setup:
        Edk2PlatformSetup().Invoke()
    elif args.update:
        Edk2Update().Invoke()
    else:
        Edk2PlatformBuild().Invoke()
