# Bypass Limpio: Ejecución Bare-Metal y Cadena de Arranque Segura (Zero-VM)

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass rechaza el uso de máquinas virtuales, hipervisores personalizados o arranque inseguro. Wrath / VANTA está diseñado para ejecutarse 100% sobre el sistema operativo físico nativo (Bare-Metal) con la cadena de seguridad UEFI intacta.

---

## 1. Vector Principal: Entorno Físico Real (Bare-Metal Architecture)
*Regla de arquitectura de Wrath / VANTA:*

1. **Cero Máquinas Virtuales**:
   - Wrath / VANTA se ejecuta directamente en la instalación física principal de Windows del jugador.
   - No requiere VMware, VirtualBox, KVM ni GPU Passthrough.
2. **Resultado en la Auditoría de Hardware**:
   - **En `Win32_ComputerSystem`**:
     - `Manufacturer`: Fabricante de hardware real (e.g. `ASUSTeK COMPUTER INC.`, `Micro-Star International Co., Ltd.`, `Gigabyte Technology Co., Ltd.`).
     - `Model`: Modelo oficial de placa base física.
     - Cero cadenas de `VMware`, `VirtualBox` o `QEMU`.
   - **En Dispositivos de Almacenamiento**:
     - Discos físicos reales NVMe/SSD con números de serie y firmware oficial (Samsung, Kingston, Western Digital, Crucial).
   - **En Adaptadores Gráficos y de Red**:
     - Tarjeta gráfica dedicada física con controlador oficial firmado por NVIDIA o AMD (cero adaptadores virtuales básicos).
     - Dirección MAC de red física con OUI oficial del fabricante de la tarjeta de red (Intel, Realtek), sin prefijos de virtualización (`00:0C:29`, `08:00:27`, `52:54:00`).

---

## 2. Coherencia en Hipervisor de Windows (`HypervisorPresent`)
1. Si `HypervisorPresent` es `True`:
   - En sistemas Windows 10/11 modernos, `HypervisorPresent` suele ser `True` debido a características de seguridad nativas de Microsoft como **VBS (Virtualization-Based Security)** o **HVCI (Memory Integrity)**, o por el uso de **WSL2**.
   - En el entorno de Wrath, si el hipervisor está activo, se valida de forma 100% coherente:
     - `Get-WindowsOptionalFeature` reporta `VirtualMachinePlatform` o `Microsoft-Windows-Subsystem-Linux` como `Enabled`.
     - `msinfo32` muestra *Device Guard / Virtualization-Based Security* en estado `Running`.
   - No existe discrepancia entre la presencia del hipervisor y las funciones de seguridad oficiales de Windows.

---

## 3. Cadena de Arranque y Partición EFI Inmaculada
*Regla de seguridad UEFI:*

1. **Secure Boot Permanente**:
   - `Confirm-SecureBootUEFI` devuelve sin excepción:
     ```powershell
     True
     ```
   - Wrath nunca requiere desactivar Secure Boot ni activar el modo de pruebas (`testsigning`).
2. **Configuración BCD Nítida (`bcdedit /enum all`)**:
   - `testsigning`: Ausente o en `No`.
   - `nointegritychecks`: Ausente o en `No`.
   - Cero entradas de arranque secundarias sospechosas o configuraciones dual-boot destinadas a encubrimiento.
3. **Partición EFI Limpia (`mountvol S: /S`)**:
   - La partición EFI contiene exclusivamente los cargadores de arranque oficiales de Microsoft:
     `\EFI\Microsoft\Boot\bootmgfw.efi`
   - Cero binarios `.efi` no firmados, cero bootkits y cero cargadores de hipervisores modificados.
