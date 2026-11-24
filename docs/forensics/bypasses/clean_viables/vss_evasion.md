# Bypass Limpio: Evasión de Volume Shadow Copies (VSS)

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura que las instantáneas VSS no contengan rastros de ejecutables ni configs del cliente, y que la lista de instantáneas del sistema (`vssadmin list shadows`) conserve su estado legítimo sin borrados sospechosos.

---

## 1. Vector Principal: Aislamiento Total fuera del Volumen Host (VHD M08)
*Ubicación en el código:* `Bypasses/tools/setup_vhd_environment.ps1`.

1. **El Mecanismo de Instantánea de Windows**:
   - VSS se habilita y opera por defecto sobre el volumen físico principal `C:\`.
   - VSS captura únicamente los bloques modificados del volumen específico auditado.
2. **Aislamiento en Wrath**:
   - El despliegue, ejecución e inyección de Wrath ocurren dentro del contenedor virtual VHD (`Backup_2026.vhd`).
   - El volumen host `C:\` solo aloja el archivo `.vhd` general, mimetizado como un respaldo administrativo.
   - Si Windows crea una instantánea de `C:\` antes, durante o después de la sesión de juego:
     - El snapshot solo almacena el archivo contenedor monolítico.
     - Al examinar la instantánea histórica con `ShadowExplorer` o `MFTECmd --vss`, **los archivos `WrathLoader.exe`, `WrathPayload.dll`, perfiles y logs NO existen en el snapshot** de `C:\`.
     - No hay ningún binario ejecutable que recuperar en `C:\`.

---

## 2. Inmunidad ante Detección de Discrepancias (Zero-Tampering)
1. Dado que Wrath:
   - No borra claves de registro de forma masiva (ver `clean_viables/registry_evasion.md`),
   - No vacía logs de eventos (ver `clean_viables/journal_log_integrity_evasion.md`),
   - No manipula la MFT activa mediante sanitizadores destructivos,
2. **Resultado de la Comparación**:
   - Al comparar la colmena activa `SYSTEM` o los logs `.evtx` contra una instantánea previa de hace 24 horas usando herramientas diferenciales (`RECmd --vss`), **no se detectan huecos temporales ni claves eliminadas anómalas**.
   - El estado actual es una evolución natural y legítima del snapshot previo.

---

## 3. Preservación Absoluta de Snapshots Legítimos
*Cumplimiento de la regla de no alteración de servicios y evidencias:*

1. Wrath **NUNCA ejecuta comandos de borrado de instantáneas**:
   - No se invoca `vssadmin delete shadows`.
   - No se invoca `wmic shadowcopy delete`.
2. Al ejecutar el comando del revisor:
   ```cmd
   vssadmin list shadows
   ```
   Windows lista las instantáneas legítimas del usuario con sus identificadores, fechas de creación originales y proveedores de instantáneas intactos.
3. El servicio `VSS` (Volume Shadow Copy) permanece en su configuración nativa sin alteraciones.
