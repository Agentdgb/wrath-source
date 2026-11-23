# Detección Forense: Hipervisores y Máquinas Virtuales (VM Detection)

## 1. Identificación y Alcance de la Metodología
- **Objetivo**: Detectar si el juego se ejecuta dentro de un entorno virtualizado (VMware, VirtualBox, QEMU/KVM) o si un hipervisor no autorizado opera por debajo del sistema operativo (Hypervisor-level cheats / Ring -1) para ocultar memoria y procesos de los escáneres convencionales.
- **Ámbitos de Inspección**:
  - Detección rápida de virtualización en 30 segundos.
  - Validación de hipervisores legítimos de Microsoft (VBS/HVCI/WSL) vs hipervisores anómalos.
  - Artefactos de controladores y hardware virtual.
  - Auditoría de la cadena de arranque UEFI y partición EFI.

---

## 2. Flujo de Inspección en Screenshares

### A. Comprobación Rápida Inicial (30 Segundos)
1. **Comando Principal en PowerShell**:
   ```powershell
   Get-CimInstance Win32_ComputerSystem | Select-Object Manufacturer, Model, HypervisorPresent
   ```
   - **`Manufacturer / Model`**: Si devuelve `VMware, Inc.`, `VirtualBox`, `QEMU`, `KVM` o `Virtual Machine`, se confirma de inmediato que el usuario está operando en una máquina virtual.
   - **`HypervisorPresent`**: Indica si un hipervisor está activo en el procesador.
2. **Systeminfo & Coreinfo**:
   - `systeminfo`: Busca al final *"A hypervisor has been detected"*.
   - `coreinfo -v` (Sysinternals): Muestra la bandera de CPU `HYPERVISOR`.

### B. Discriminación: Hipervisor Legítimo vs Hipervisor Sospechoso
En Windows 10/11, el hipervisor nativo de Microsoft puede estar activo legítimamente por:
- VBS (Virtualization-Based Security) / HVCI (Integridad de Memoria).
- WSL2 (Windows Subsystem for Linux), Windows Sandbox o Docker.

**Verificación Cruzada del Revisor**:
1. Inspección de BCD (`bcdedit /enum all`):
   - Verifica `hypervisorlaunchtype` (`Auto` / `Off`).
   - Busca banderas de prueba de firmas: `testsigning`, `nointegritychecks`.
2. Inspección de Características Opcionales:
   ```powershell
   Get-WindowsOptionalFeature -Online | Where-Object FeatureName -match "Hyper|VirtualMachine|Sandbox"
   ```
3. **Criterio de Alerta**: Si `HypervisorPresent = True`, pero VBS está desactivado y el usuario no tiene WSL, Docker ni Sandbox habilitados, se cataloga como **Hipervisor No Explicado (Unexplained Hypervisor)** y se exige auditoría de arranque.

### C. Trazas de Controladores y Hardware Virtual
1. **Drivers y Servicios en Registro** (`HKLM\SYSTEM\CurrentControlSet\Services`):
   - VMware: `vmci`, `vmhgfs`, `vmmouse`, `vmusb`.
   - VirtualBox: `VBoxGuest`, `vboxdrv`, `VBoxMouse`, `VBoxSF`.
   - KVM/QEMU: `virtio*`, `qemu-ga`.
2. **Dispositivos y GPU**:
   - Discos: `VBOX HARDDISK`, `QEMU HARDDISK`, `VMware Virtual disk`.
   - GPU: `VMware SVGA`, `Microsoft Basic Display Adapter` con GPU passthrough.
3. **Prefijos MAC (OUI de Virtualización)**:
   - `00:15:5D` (Hyper-V)
   - `00:0C:29`, `00:50:56`, `00:05:69` (VMware)
   - `08:00:27` (VirtualBox)
   - `52:54:00` (QEMU/KVM)

### D. Auditoría de Arranque y Partición EFI
1. **Comprobación de Secure Boot**:
   ```powershell
   Confirm-SecureBootUEFI
   ```
2. **Inspección de la Partición EFI**:
   ```cmd
   mountvol S: /S
   dir S:\EFI /s
   mountvol S: /D
   ```
   - **Banderas de Baneo**:
     - `Secure Boot` deshabilitado (`False`) combinado con archivos `.efi` no firmados o carpetas extrañas fuera de `\EFI\Microsoft` y `\EFI\Boot`.
     - Entradas de arranque duales en BCD para sistemas alternativos de juego.
