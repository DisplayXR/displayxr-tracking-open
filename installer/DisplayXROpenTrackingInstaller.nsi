; DisplayXR open camera tracking provider installer (Windows x64)
; Copyright 2026, The DisplayXR Project
; SPDX-License-Identifier: BSL-1.0
;
; Installs DisplayXR-OpenTracking.dll (+ ONNX Runtime, the model pack manifest and the four
; ONNX models, all next to it) into $PROGRAMFILES64\DisplayXR\Plugins\OpenTracking and writes
; the two provider rows the runtime discovers it by (one binary, two roles):
;
;   HKLM\Software\DisplayXR\TrackingProviders\open-tracking       viewer tracking (head + eyes)
;   HKLM\Software\DisplayXR\InputProviders\open-tracking-hands    hand tracking (ADR-034)
;
; (runtime specs tracking-provider-discovery.md and input-provider-discovery.md section 3:
; Binary, DisplayName, Vendor, ProbeOrder, Version.) Version is written LAST in each row,
; after the allowlist row, so the runtime never reads a row whose binary it would refuse.
;
; Order-independent: no runtime prerequisite (the bundle installs components in any order).
; The runtime scans providers when a process starts, so a running DisplayXR service is
; restarted after the install (and the uninstall) to pick the change up, at Medium integrity
; through explorer.exe exactly as the runtime installer does; /NOSTART skips that (the bundle
; restarts the service once at the end of its chain).
;
; Files in use: the service and the tracking host are stopped first; an OpenXR app that runs
; the runtime in-process may still map the DLLs, so a file that cannot be overwritten is
; renamed aside (Windows allows renaming a mapped image) and deleted at the next reboot.
;
; The uninstaller removes only this provider's rows, files and allowlist row, never the
; runtime or a per-user setting.
;
; Build-time definitions: VERSION, DIST_DIR (a `cmake --install` tree that includes the
; models, -DOPENTRACK_INSTALL_MODELS_DIR), OUTPUT_DIR, ALLOWLIST_NSH (see below) and SIGN_CMD
; (optional; see "Code signing").
;
; ADR-054 N4b: the runtime's provenance gate ENFORCES on Windows: the provider DLL must be
; Authenticode-signed or have its SHA-256 under HKLM\Software\DisplayXR\Trust\Allowlist, or
; the loader refuses it as `unverified`. Unsigned builds therefore carry the digest of exactly
; the DLL they ship (installer/gen_allowlist_nsh.cmake -> ALLOWLIST_NSH). An allowlisted (or
; signed) provider also gets the runtime's `component` consent verdict for the shared camera:
; no per-executable prompt.

!ifndef VERSION
	!define VERSION "0.0.0"
!endif
!ifndef DIST_DIR
	!error "pass -DDIST_DIR=<cmake install tree>"
!endif
!ifndef OUTPUT_DIR
	!define OUTPUT_DIR "."
!endif

; Code signing (SIGN_CMD; undefined or empty = unsigned build). No certificate or secret lives
; here: the signing provider passes a per-file signer command that takes the file path as its
; last argument. The installer .exe is signed via !finalize; the UNINSTALLER via a two-pass
; build (compile an INNER installer whose only job is to emit the uninstaller to %TEMP%, run
; it, sign that file, File-include it in the real pass), because !uninstfinalize can leave the
; uninstaller with the installer's certificate-table pointer dangling past its end. Same
; recipe as the DisplayXR runtime and the conversion providers. The provider DLL itself is
; signed BEFORE makensis runs, by the caller, and the allowlist row (ALLOWLIST_NSH) is
; generated from the signed bytes.
!ifndef INNER
	!ifdef SIGN_CMD
		!if "${SIGN_CMD}" != ""
			!ifdef ALLOWLIST_NSH
				!define OT_INNER_ALLOWLIST '"-DALLOWLIST_NSH=${ALLOWLIST_NSH}"'
			!else
				!define OT_INNER_ALLOWLIST ''
			!endif
			!finalize '${SIGN_CMD} "%1"'
			!makensis '-DINNER "-DVERSION=${VERSION}" "-DDIST_DIR=${DIST_DIR}" ${OT_INNER_ALLOWLIST} "DisplayXROpenTrackingInstaller.nsi"' = 0
			; makensis runs in the script's own directory, so the inner pass names the script by
			; file. Its .onInit writes %TEMP%\OpenTracking_Uninstall.exe, then Quit (exit 2).
			!system '"$%TEMP%\DisplayXROpenTracking_inner.exe"' = 2
			!system '${SIGN_CMD} "$%TEMP%\OpenTracking_Uninstall.exe"' = 0
			!define USE_PRESIGNED_UNINST
		!endif
	!endif
!endif

!ifdef ALLOWLIST_NSH
	!include "${ALLOWLIST_NSH}"
!else
	!warning "ALLOWLIST_NSH not defined: this installer will write no provenance allowlist row, and an enforcing runtime will refuse the unsigned provider DLL"
	!macro DXR_ALLOWLIST_WRITE
	!macroend
	!macro DXR_ALLOWLIST_REMOVE
	!macroend
!endif

!define BINARY "DisplayXR-OpenTracking.dll"
!define VIEWER_ID "open-tracking"
!define HANDS_ID "open-tracking-hands"
!define VIEWER_ROW "Software\DisplayXR\TrackingProviders\${VIEWER_ID}"
!define HANDS_ROW "Software\DisplayXR\InputProviders\${HANDS_ID}"
!define MARKER "Software\DisplayXR\Plugins\OpenTracking"
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXROpenTracking"
!define ALLOWLIST "Software\DisplayXR\Trust\Allowlist"

Name "DisplayXR Open Tracking ${VERSION}"
!ifdef INNER
	OutFile "$%TEMP%\DisplayXROpenTracking_inner.exe"
	RequestExecutionLevel user
!else
	OutFile "${OUTPUT_DIR}\DisplayXROpenTrackingSetup-${VERSION}.exe"
	RequestExecutionLevel admin
!endif
InstallDir "$PROGRAMFILES64\DisplayXR\Plugins\OpenTracking"
InstallDirRegKey HKLM "${MARKER}" "InstallPath"
ShowInstDetails show
ShowUninstDetails show
; A file that cannot be written aborts the install (non-zero exit) instead of being skipped
; under a new registry Version.
AllowSkipFiles off
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "DisplayXR Open Tracking"
VIAddVersionKey "FileDescription" "DisplayXR open camera tracking provider installer"
VIAddVersionKey "CompanyName" "DisplayXR"
VIAddVersionKey "LegalCopyright" "The DisplayXR Project (BSL-1.0)"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"

!include "x64.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

; "1" when this run stopped a running displayxr-service.exe (it is started again at the end).
Var ServiceWasRunning
; "1" with /NOSTART: never start the service (the bundle starts it once at the end of its chain).
Var NoStart
; The runtime's install directory (where displayxr-service.exe lives), "" when not installed.
Var RuntimeDir

; End a process by image name. Sets $ServiceWasRunning for the service. Quiet when it is not
; running (taskkill exit 128). $0 / $1 are preserved.
!macro OT_STOP_IMAGE IMAGE IS_SERVICE
	Push $0
	Push $1
	nsExec::ExecToStack 'taskkill /f /im ${IMAGE}'
	Pop $0
	Pop $1
	${If} $0 == "0"
		DetailPrint "Stopped ${IMAGE}."
		${If} ${IS_SERVICE} == 1
			StrCpy $ServiceWasRunning "1"
		${EndIf}
	${ElseIf} $0 != "128"
		DetailPrint "Could not stop ${IMAGE} (taskkill exit $0): $1"
	${EndIf}
	Pop $1
	Pop $0
!macroend

; Replace-or-remove helper: a file an in-process app still maps cannot be overwritten or
; deleted, but it can be renamed. Move it aside and delete it at the next reboot.
!macro OT_MOVE_ASIDE_IF_LOCKED PATH
	${If} ${FileExists} "${PATH}"
		ClearErrors
		Delete "${PATH}"
		${If} ${Errors}
			Delete "${PATH}.old"
			ClearErrors
			Rename "${PATH}" "${PATH}.old"
			${IfNot} ${Errors}
				DetailPrint "${PATH} is in use: moved aside, removed at the next reboot."
				Delete /REBOOTOK "${PATH}.old"
			${EndIf}
		${EndIf}
	${EndIf}
!macroend

!macro OT_STOP_RUNTIME_PROCESSES
	StrCpy $ServiceWasRunning "0"
	!insertmacro OT_STOP_IMAGE "displayxr-service.exe" 1
	; The tracking host (the viewer role's process) dies with the service's job object; sweep a
	; straggler so the DLL is free.
	!insertmacro OT_STOP_IMAGE "displayxr-tracking-host.exe" 0
!macroend

; Start the service again if this run stopped it, at Medium integrity (explorer.exe), unless
; /NOSTART. Never with --autostart (that switch is the logon marker).
!macro OT_RESTART_SERVICE
	${If} $ServiceWasRunning == "1"
		StrCpy $ServiceWasRunning "0"
		${If} $NoStart == "1"
			DetailPrint "Not restarting the DisplayXR Service (/NOSTART)."
		${ElseIf} $RuntimeDir != ""
		${AndIf} ${FileExists} "$RuntimeDir\displayxr-service.exe"
			DetailPrint "Restarting the DisplayXR Service so it finds the provider change..."
			Exec 'explorer.exe "$RuntimeDir\displayxr-service.exe"'
		${EndIf}
	${EndIf}
!macroend

Function .onInit
!ifdef INNER
	; Inner signing pass only: emit the uninstaller (the binary the outer pass signs and
	; File-includes), then bail: no UI, no install, no registry. Exits 2 by design.
	SetSilent silent
	WriteUninstaller "$%TEMP%\OpenTracking_Uninstall.exe"
	Quit
!endif
	${IfNot} ${RunningX64}
		MessageBox MB_ICONSTOP|MB_OK "DisplayXR Open Tracking needs 64-bit Windows." /SD IDOK
		SetErrorLevel 3
		Abort
	${EndIf}
	SetRegView 64
	ReadRegStr $RuntimeDir HKLM "Software\DisplayXR\Runtime" "InstallPath"
	StrCpy $ServiceWasRunning "0"
	StrCpy $NoStart "0"
	${GetParameters} $R0
	ClearErrors
	${GetOptions} $R0 "/NOSTART" $R1
	${IfNot} ${Errors}
		StrCpy $NoStart "1"
	${EndIf}
FunctionEnd

Function .onInstFailed
	; An aborted install leaves the machine as it found it, running service included.
	!insertmacro OT_RESTART_SERVICE
FunctionEnd

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Open Tracking provider"
	SectionIn RO
	SetRegView 64
	${If} $RuntimeDir == ""
		DetailPrint "The DisplayXR runtime is not installed yet: the provider is registered and is used once it is."
	${EndIf}

	!insertmacro OT_STOP_RUNTIME_PROCESSES
	!insertmacro OT_MOVE_ASIDE_IF_LOCKED "$INSTDIR\${BINARY}"
	!insertmacro OT_MOVE_ASIDE_IF_LOCKED "$INSTDIR\onnxruntime.dll"

	SetOutPath "$INSTDIR"
	File /r /x examples "${DIST_DIR}\*.*"
!ifdef USE_PRESIGNED_UNINST
	File "/oname=Uninstall.exe" "$%TEMP%\OpenTracking_Uninstall.exe"
!else
	WriteUninstaller "$INSTDIR\Uninstall.exe"
!endif

	; An upgrade: the previous version's allowlist row names bytes that are gone.
	ReadRegStr $0 HKLM "${MARKER}" "AllowlistSha256"
	${If} $0 != ""
		DeleteRegValue HKLM "${ALLOWLIST}" "$0"
	${EndIf}
	; ADR-054 N4b: the digest of the DLL just written, before either row is marked complete.
	!insertmacro DXR_ALLOWLIST_WRITE
!ifdef OT_ALLOWLIST_SHA256
	WriteRegStr HKLM "${MARKER}" "AllowlistSha256" "${OT_ALLOWLIST_SHA256}"
!endif

	; The viewer-tracking row. ProbeOrder 100: ahead of the simulated provider (200).
	WriteRegStr   HKLM "${VIEWER_ROW}" "Binary"          "$INSTDIR\${BINARY}"
	WriteRegStr   HKLM "${VIEWER_ROW}" "DisplayName"     "DisplayXR Open Tracking"
	WriteRegStr   HKLM "${VIEWER_ROW}" "Vendor"          "DisplayXR"
	WriteRegStr   HKLM "${VIEWER_ROW}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
	WriteRegDWORD HKLM "${VIEWER_ROW}" "ProbeOrder"      100
	WriteRegStr   HKLM "${VIEWER_ROW}" "Version"         "${VERSION}"

	; The hands row. ProbeOrder 100: behind a dedicated hand tracker (50), ahead of the simulated
	; input providers (190 / 200).
	WriteRegStr   HKLM "${HANDS_ROW}" "Binary"          "$INSTDIR\${BINARY}"
	WriteRegStr   HKLM "${HANDS_ROW}" "DisplayName"     "DisplayXR Open Tracking: hands"
	WriteRegStr   HKLM "${HANDS_ROW}" "Vendor"          "DisplayXR"
	WriteRegStr   HKLM "${HANDS_ROW}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
	WriteRegDWORD HKLM "${HANDS_ROW}" "ProbeOrder"      100
	WriteRegStr   HKLM "${HANDS_ROW}" "Version"         "${VERSION}"

	; The install marker the bundle and the catalog read.
	WriteRegStr HKLM "${MARKER}" "InstallPath" "$INSTDIR"
	WriteRegStr HKLM "${MARKER}" "Version" "${VERSION}"

	WriteRegStr   HKLM "${UNINST}" "DisplayName"     "DisplayXR Open Tracking"
	WriteRegStr   HKLM "${UNINST}" "DisplayVersion"  "${VERSION}"
	WriteRegStr   HKLM "${UNINST}" "Publisher"       "DisplayXR"
	WriteRegStr   HKLM "${UNINST}" "InstallLocation" "$INSTDIR"
	WriteRegStr   HKLM "${UNINST}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
	WriteRegStr   HKLM "${UNINST}" "QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
	WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
	WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
	${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
	IntFmt $0 "0x%08X" $0
	WriteRegDWORD HKLM "${UNINST}" "EstimatedSize" "$0"

	!insertmacro OT_RESTART_SERVICE
SectionEnd

Function un.onInit
	SetRegView 64
	ReadRegStr $RuntimeDir HKLM "Software\DisplayXR\Runtime" "InstallPath"
	StrCpy $ServiceWasRunning "0"
	StrCpy $NoStart "0"
FunctionEnd

Section "Uninstall"
	SetRegView 64
	!insertmacro OT_STOP_RUNTIME_PROCESSES
	; The rows first, so nothing finds a binary that is about to go.
	DeleteRegKey HKLM "${VIEWER_ROW}"
	DeleteRegKey HKLM "${HANDS_ROW}"
	DeleteRegKey /ifempty HKLM "Software\DisplayXR\TrackingProviders"
	DeleteRegKey /ifempty HKLM "Software\DisplayXR\InputProviders"
	; ADR-054 N4b: only the allowlist row this installer wrote.
	ReadRegStr $0 HKLM "${MARKER}" "AllowlistSha256"
	${If} $0 != ""
		DeleteRegValue HKLM "${ALLOWLIST}" "$0"
	${EndIf}
	!insertmacro DXR_ALLOWLIST_REMOVE
	DeleteRegKey HKLM "${MARKER}"
	DeleteRegKey /ifempty HKLM "Software\DisplayXR\Plugins"
	DeleteRegKey HKLM "${UNINST}"

	!insertmacro OT_MOVE_ASIDE_IF_LOCKED "$INSTDIR\${BINARY}"
	!insertmacro OT_MOVE_ASIDE_IF_LOCKED "$INSTDIR\onnxruntime.dll"
	Delete "$INSTDIR\*.onnx"
	Delete "$INSTDIR\*.models.json"
	RMDir /r "$INSTDIR\licenses"
	Delete "$INSTDIR\Uninstall.exe"
	RMDir "$INSTDIR"
	RMDir "$PROGRAMFILES64\DisplayXR\Plugins"

	!insertmacro OT_RESTART_SERVICE
SectionEnd
