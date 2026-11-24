# Repositorio Forense — Detecciones y Bypasses (Wrath / VANTA)

Estructura organizada para clasificar métodos de detección utilizados por herramientas de screenshare / análisis forense (Echo, Paladin, Ocean, Avenge, inspectores USN/MFT, Prefetch, BAM, PCA, memoria) y sus respectivos bypasses nivelados.

---

## Organización

```
Bypasses/forensics/
├── detecciones/              # Qué y cómo buscan las herramientas forenses (vectores, firmas, telemetría)
└── bypasses/
    ├── clean_viables/        # Bypasses que sí sirven (sin parar servicios, sin desincronizar, compatibles con Wrath)
    └── obsoletos_blatant/    # Bypasses descartados (viejos, blatants, parada de servicios, banderas rojas)
```

---

## Reglas de Admisión

1. **Servicios Intactos**: Prohibido parar/deshabilitar servicios (`DPS`, `PcaSvc`, `EventLog`, `SysMain`, etc.).
2. **Cero Blatant / Obsoletos**: Sin borrado ciego de logs, 0-bytes, desincronización de timestamps o discrepancias PEB vs VAD.
3. **Alineación con Wrath**: Compatibilidad estricta con el loader DX11, payload Fabric 1.21.x y suite de trace teardown.
4. **Cero Doble Clic (Anti-UserAssist / Anti-LastActivityView)**: Prohibido ejecutar inyectores/loaders mediante doble clic manual en el Explorador de Windows antes o durante el juego. Debe operar como servicio persistente desde el arranque del sistema (`boot time`) o desacoplado del Shell.
5. **Cero Mods en `.minecraft\mods`**: Prohibido colocar archivos `.jar`/`.zip` del cliente en la carpeta de mods. Detección directa por herramientas como `MeowModAnalyzer.ps1` y `JARParser`. La inyección debe ser 100% nativa en memoria de `javaw.exe` vía `SEC_IMAGE`.
6. **Cero Ejecución Pre-Juego (Anti-Timeline Clustering)**: Prohibido abrir cualquier binario o ejecutable inusual (`.exe`, scripts o utilidades camufladas) en la ventana previa a iniciar Minecraft (5-10 min antes de `javaw.exe`). Los evaluadores analizan clústeres temporales en `LastActivityView`, `BAM` y `Prefetch`; cualquier `.exe` previo levanta sospecha directa. Toda inyección debe partir de controladores de hardware o procesos legítimos preexistentes desde el arranque del sistema (`boot time`).

---

## Índice Temático (38 Módulos)

| # | Tema / Vector | Detección | Bypass Limpio (Wrath) | Obsoleto / Flagrante |
|---|---|---|---|---|
| 1 | **MFTECmd & $MFT** | [mft_mftecmd.md](detecciones/mft_mftecmd.md) | [mft_evasion.md](bypasses/clean_viables/mft_evasion.md) | [mft_borrado_simple.md](bypasses/obsoletos_blatant/mft_borrado_simple.md) |
| 2 | **SRUM & SrumECmd** | [srum_srumecmd.md](detecciones/srum_srumecmd.md) | [srum_evasion.md](bypasses/clean_viables/srum_evasion.md) | [srum_destructivo.md](bypasses/obsoletos_blatant/srum_destructivo.md) |
| 3 | **RegistryExplorer & rla** | [registry_explorer_rla.md](detecciones/registry_explorer_rla.md) | [registry_evasion.md](bypasses/clean_viables/registry_evasion.md) | [registry_regdelete_blatant.md](bypasses/obsoletos_blatant/registry_regdelete_blatant.md) |
| 4 | **NTFS $LogFile & Tx** | [ntfs_logfile.md](detecciones/ntfs_logfile.md) | [logfile_evasion.md](bypasses/clean_viables/logfile_evasion.md) | [logfile_destructivo.md](bypasses/obsoletos_blatant/logfile_destructivo.md) |
| 5 | **ShimCache (AppCompat)** | [shimcache_appcompatcache.md](detecciones/shimcache_appcompatcache.md) | [shimcache_evasion.md](bypasses/clean_viables/shimcache_evasion.md) | [shimcache_vaciado_blatant.md](bypasses/obsoletos_blatant/shimcache_vaciado_blatant.md) |
| 6 | **AmCache (`Amcache.hve`)** | [amcache_amcacheparser.md](detecciones/amcache_amcacheparser.md) | [amcache_evasion.md](bypasses/clean_viables/amcache_evasion.md) | [amcache_vaciado_blatant.md](bypasses/obsoletos_blatant/amcache_vaciado_blatant.md) |
| 7 | **DLL Hijacking & Proxying** | [dll_hijacking_detection.md](detecciones/dll_hijacking_detection.md) | [dll_hijacking_evasion.md](bypasses/clean_viables/dll_hijacking_evasion.md) | [dll_hijacking_blatant.md](bypasses/obsoletos_blatant/dll_hijacking_blatant.md) |
| 8 | **MRUHunt & PowerShell-Hunter** | [mru_powershell_hunter.md](detecciones/mru_powershell_hunter.md) | [mru_jumplists_evasion.md](bypasses/clean_viables/mru_jumplists_evasion.md) | [mru_vaciado_blatant.md](bypasses/obsoletos_blatant/mru_vaciado_blatant.md) |
| 9 | **Fileless Execution (EventLogs)**| [fileless_execution_detection.md](detecciones/fileless_execution_detection.md) | [fileless_evasion.md](bypasses/clean_viables/fileless_evasion.md) | [fileless_blatant.md](bypasses/obsoletos_blatant/fileless_blatant.md) |
| 10 | **Bypasses, Drivers & Services** | [bypass_artifacts_detection.md](detecciones/bypass_artifacts_detection.md) | [kernel_driver_service_evasion.md](bypasses/clean_viables/kernel_driver_service_evasion.md) | [kernel_driver_service_blatant.md](bypasses/obsoletos_blatant/kernel_driver_service_blatant.md) |
| 11 | **Live CMD / PowerShell Audit** | [cmd_powershell_live_audit.md](detecciones/cmd_powershell_live_audit.md) | [live_audit_evasion.md](bypasses/clean_viables/live_audit_evasion.md) | [live_audit_blatant.md](bypasses/obsoletos_blatant/live_audit_blatant.md) |
| 12 | **Journal & Event Log Integrity**| [journal_log_manipulation_detection.md](detecciones/journal_log_manipulation_detection.md) | [journal_log_integrity_evasion.md](bypasses/clean_viables/journal_log_integrity_evasion.md) | [journal_log_tampering_blatant.md](bypasses/obsoletos_blatant/journal_log_tampering_blatant.md) |
| 13 | **Kernel Live Dump & RedLotus** | [kernel_live_dump.md](detecciones/kernel_live_dump.md) | [kernel_dump_evasion.md](bypasses/clean_viables/kernel_dump_evasion.md) | [kernel_dump_blatant.md](bypasses/obsoletos_blatant/kernel_dump_blatant.md) |
| 14 | **Chainsaw (Event Log Hunt)** | [chainsaw_event_logs.md](detecciones/chainsaw_event_logs.md) | [chainsaw_evasion.md](bypasses/clean_viables/chainsaw_evasion.md) | [chainsaw_blatant.md](bypasses/obsoletos_blatant/chainsaw_blatant.md) |
| 15 | **Velociraptor (VQL & YARA)** | [velociraptor_vql_yara.md](detecciones/velociraptor_vql_yara.md) | [velociraptor_vql_evasion.md](bypasses/clean_viables/velociraptor_vql_evasion.md) | [velociraptor_blatant.md](bypasses/obsoletos_blatant/velociraptor_blatant.md) |
| 16 | **RAM Dump & CMD History** | [ram_dump_cmd_commands.md](detecciones/ram_dump_cmd_commands.md) | [ram_dump_cmd_evasion.md](bypasses/clean_viables/ram_dump_cmd_evasion.md) | [ram_dump_cmd_blatant.md](bypasses/obsoletos_blatant/ram_dump_cmd_blatant.md) |
| 17 | **Volume Shadow Copies (VSS)** | [vss_volume_shadow_copies.md](detecciones/vss_volume_shadow_copies.md) | [vss_evasion.md](bypasses/clean_viables/vss_evasion.md) | [vss_destructivo_blatant.md](bypasses/obsoletos_blatant/vss_destructivo_blatant.md) |
| 18 | **NTFS $i30 Attribute (Index)** | [ntfs_i30_indexes.md](detecciones/ntfs_i30_indexes.md) | [i30_index_evasion.md](bypasses/clean_viables/i30_index_evasion.md) | [i30_borrado_naive_blatant.md](bypasses/obsoletos_blatant/i30_borrado_naive_blatant.md) |
| 19 | **NACIO Kernel Scanner** | [nacio_kernel_scanner.md](detecciones/nacio_kernel_scanner.md) | [nacio_kernel_scanner_evasion.md](bypasses/clean_viables/nacio_kernel_scanner_evasion.md) | [kernel_byovd_blatant.md](bypasses/obsoletos_blatant/kernel_byovd_blatant.md) |
| 20 | **Process Memory Strings** | [magnet_bstrings_base64hunter.md](detecciones/magnet_bstrings_base64hunter.md) | [process_strings_base64_evasion.md](bypasses/clean_viables/process_strings_base64_evasion.md) | [base64_strings_blatant.md](bypasses/obsoletos_blatant/base64_strings_blatant.md) |
| 21 | **Execution & File Existence** | [execution_and_file_existence_artifacts.md](detecciones/execution_and_file_existence_artifacts.md) | [execution_and_file_existence_evasion.md](bypasses/clean_viables/execution_and_file_existence_evasion.md) | [execution_file_existence_blatant.md](bypasses/obsoletos_blatant/execution_file_existence_blatant.md) |
| 22 | **Registry Explorer Regex** | [registry_explorer_regex_search.md](detecciones/registry_explorer_regex_search.md) | [registry_regex_search_evasion.md](bypasses/clean_viables/registry_regex_search_evasion.md) | [registry_regex_blatant.md](bypasses/obsoletos_blatant/registry_regex_blatant.md) |
| 23 | **Evidence of Existence Sync** | [evidence_of_existence_comprehensive.md](detecciones/evidence_of_existence_comprehensive.md) | [evidence_of_existence_clean.md](bypasses/clean_viables/evidence_of_existence_clean.md) | [evidence_of_existence_blatant.md](bypasses/obsoletos_blatant/evidence_of_existence_blatant.md) |
| 24 | **WMIC & Spoofed Extensions** | [wmic_and_spoofed_extensions.md](detecciones/wmic_and_spoofed_extensions.md) | [process_creation_and_extension_clean.md](bypasses/clean_viables/process_creation_and_extension_clean.md) | [wmic_spoofed_extensions_blatant.md](bypasses/obsoletos_blatant/wmic_spoofed_extensions_blatant.md) |
| 25 | **Scheduled Tasks Detection** | [scheduled_tasks_detection.md](detecciones/scheduled_tasks_detection.md) | [scheduled_tasks_clean_evasion.md](bypasses/clean_viables/scheduled_tasks_clean_evasion.md) | [scheduled_tasks_blatant.md](bypasses/obsoletos_blatant/scheduled_tasks_blatant.md) |
| 26 | **Memory Editing Residue (GUI)**| [system_informer_memory_editor_residue.md](detecciones/system_informer_memory_editor_residue.md) | [memory_editing_gui_evasion.md](bypasses/clean_viables/memory_editing_gui_evasion.md) | [process_hacker_gui_editing_blatant.md](bypasses/obsoletos_blatant/process_hacker_gui_editing_blatant.md) |
| 27 | **Usermode Hiders & In-Memory**| [usermode_hiders_and_injection.md](detecciones/usermode_hiders_and_injection.md) | [zero_hider_memory_clean.md](bypasses/clean_viables/zero_hider_memory_clean.md) | [usermode_hiders_blatant.md](bypasses/obsoletos_blatant/usermode_hiders_blatant.md) |
| 28 | **Foundational Forensics & USB**| [foundational_windows_forensics.md](detecciones/foundational_windows_forensics.md) | [foundational_forensics_evasion.md](bypasses/clean_viables/foundational_forensics_evasion.md) | [usb_and_regback_blatant.md](bypasses/obsoletos_blatant/usb_and_regback_blatant.md) |
| 29 | **Suspended Threads & Logging** | [suspended_threads_and_inheritance_bypasses.md](detecciones/suspended_threads_and_inheritance_bypasses.md) | [service_thread_integrity_clean.md](bypasses/clean_viables/service_thread_integrity_clean.md) | [suspended_threads_blatant.md](bypasses/obsoletos_blatant/suspended_threads_blatant.md) |
| 30 | **CLR Logs (.NET Runtime)** | [clr_dotnet_runtime_traces.md](detecciones/clr_dotnet_runtime_traces.md) | [native_cpp_zero_clr_clean.md](bypasses/clean_viables/native_cpp_zero_clr_clean.md) | [dotnet_cheats_blatant.md](bypasses/obsoletos_blatant/dotnet_cheats_blatant.md) |
| 31 | **Hidden Accounts (Winlogon)** | [hidden_accounts_detection.md](detecciones/hidden_accounts_detection.md) | [user_account_integrity_clean.md](bypasses/clean_viables/user_account_integrity_clean.md) | [hidden_accounts_blatant.md](bypasses/obsoletos_blatant/hidden_accounts_blatant.md) |
| 32 | **Hypervisor & VM Detection** | [hypervisor_vm_detection.md](detecciones/hypervisor_vm_detection.md) | [bare_metal_userland_clean.md](bypasses/clean_viables/bare_metal_userland_clean.md) | [vm_hypervisor_blatant.md](bypasses/obsoletos_blatant/vm_hypervisor_blatant.md) |
| 33 | **Common Checker Mistakes** | [common_checker_mistakes.md](detecciones/common_checker_mistakes.md) | [anti_checker_methodology_clean.md](bypasses/clean_viables/anti_checker_methodology_clean.md) | [checker_manipulation_blatant.md](bypasses/obsoletos_blatant/checker_manipulation_blatant.md) |
| 34 | **Hiding Strings in Java / JVM**| [hiding_strings_jvm_constant_pool.md](detecciones/hiding_strings_jvm_constant_pool.md) | [hiding_strings_native_clean.md](bypasses/clean_viables/hiding_strings_native_clean.md) | [hiding_strings_fragmentation_blatant.md](bypasses/obsoletos_blatant/hiding_strings_fragmentation_blatant.md) |
| 35 | **Process Hollowing** | [process_hollowing_detection.md](detecciones/process_hollowing_detection.md) | [process_hollow_zero_clean.md](bypasses/clean_viables/process_hollow_zero_clean.md) | [process_hollowing_blatant.md](bypasses/obsoletos_blatant/process_hollowing_blatant.md) |
| 36 | **$Recycle.Bin ($I/$R)** | [recycle_bin_forensics.md](detecciones/recycle_bin_forensics.md) | [recycle_bin_zero_clean.md](bypasses/clean_viables/recycle_bin_zero_clean.md) | [recycle_bin_borrado_blatant.md](bypasses/obsoletos_blatant/recycle_bin_borrado_blatant.md) |
| 37 | **Shellcode & Unbacked Memory** | [shellcode_unbacked_memory_detection.md](detecciones/shellcode_unbacked_memory_detection.md) | [shellcode_zero_unbacked_clean.md](bypasses/clean_viables/shellcode_zero_unbacked_clean.md) | [shellcode_rwx_blatant.md](bypasses/obsoletos_blatant/shellcode_rwx_blatant.md) |
| 38 | **Pre-Launch Timeline Clustering**| [pre_launch_timeline_anomaly.md](detecciones/pre_launch_timeline_anomaly.md) | [pre_launch_zero_exe_clean.md](bypasses/clean_viables/pre_launch_zero_exe_clean.md) | [pre_launch_exe_blatant.md](bypasses/obsoletos_blatant/pre_launch_exe_blatant.md) |
