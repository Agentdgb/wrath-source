# Bypass Obsoleto / Blatant: Juego en Máquinas Virtuales e Hipervisores de Cheats

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Intentar jugar dentro de una máquina virtual o recurrir a hipervisores de kernel modificados (Ring -1) garantiza el baneo automático en los primeros 30 segundos de cualquier screenshare.

---

## 1. Jugar dentro de una Máquina Virtual (VMware, VirtualBox, KVM con Passthrough)
- **Técnica errónea**: Instalar el juego dentro de una máquina virtual (con GPU Passthrough para buen rendimiento) y ejecutar el cheat en el host, creyendo que el revisor solo tiene acceso a la VM "limpia".
- **Por qué falla ante los analistas (Baneo Inmediato)**:
  1. **Comprobación en 30 Segundos**:
     - El revisor ejecuta:
       ```powershell
       Get-CimInstance Win32_ComputerSystem | Select Manufacturer, Model
       ```
     - La salida imprime en pantalla:
       `Manufacturer: VMware, Inc.` o `Model: VirtualBox` / `QEMU Virtual Machine`.
  2. **Regla de Baneo Directo**:
     - En los reglamentos competitivos (Echo, Paladin, ligas de FiveM y torneos de Minecraft), **competir desde una máquina virtual está terminantemente prohibido**.
     - Es causal de baneo permanente directo o exigencia de revisión obligatoria del equipo anfitrión físico. Si el usuario se niega a revisar el host: ban definitivo.
  3. **Dirección MAC y Controladores**:
     - Las interfaces de red exhiben prefijos OUI conocidos (`00:0C:29`, `08:00:27`, `52:54:00`).
     - Los servicios `vmmouse`, `VBoxGuest` o `virtio` en el registro confirman el entorno virtual.

---

## 2. Hipervisores de Trampas (Type-1 / Ring -1 Hypervisors) y Bootkits UEFI
- **Técnica errónea**: Cargar un hipervisor personalizado (e.g. SimpleVisor, Dma-virtualizer) en el arranque del equipo para ocultar memoria física.
- **Por qué falla ante la auditoría de arranque**:
  1. **Requerimiento de Desactivar Secure Boot**:
     - Ningún hipervisor personalizado de trampas posee una firma digital válida de Microsoft compatible con Secure Boot.
     - Requieren desactivar Secure Boot (`Confirm-SecureBootUEFI` devuelve `False`) o activar `testsigning = Yes` en el BCD.
  2. **Inspección de la Partición EFI (`mountvol S: /S`)**:
     - El revisor monta la partición EFI del sistema y revisa los archivos:
       Encuentra binarios `.efi` no firmados o carpetas extrañas junto a `\EFI\Boot\bootx64.efi`.
  3. **Hipervisor No Explicado**:
     - Si `systeminfo` reporta que hay un hipervisor presente, pero el usuario no tiene VBS, WSL ni Sandbox activos en Windows, se declara inmediatamente **Hipervisor No Autorizado / Manipulación de Kernel (Ban Permanente)**.
