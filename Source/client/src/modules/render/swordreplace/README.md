# Sword cosmetics

SwordReplace conserva su nombre de configuración y ofrece exactamente 20
cosméticos originales de GodWeer. Katana es la opción predeterminada.

Los modelos, texturas, animaciones y 20 vistas previas se incluyen como datos
C++ en SwordReplaceAssets.cpp. tools/build_cosmetic_assets.py reproduce esos
datos a partir de GodWeer.zip y el adaptador compilado CosmeticBridge.class.
Las vistas previas muestran la geometría y las texturas reales de cada modelo.

No se instala ningún ZIP, JAR o mod externo. El módulo define en la JVM un
adaptador mínimo de interfaces incluido en la DLL y registra su método nativo.
Toda la selección, entrega de recursos y lógica de activación vive en C++.
El proveedor de recursos en memoria usa el cargador de clases de Minecraft y
se monta exclusivamente al activar el cosmético; no depende de JVMTI ni de
modificar/clonar métodos del juego. Tampoco modifica stacks ni inventarios.

Al activar por primera vez, Minecraft carga los 20 modelos bajo identificadores
propios, una sola vez. El módulo conecta el selector a la función de búsqueda
de ItemModelManager mediante un adaptador nativo normal de java.util.function.Function.
Cambiar de espada solo selecciona otro modelo ya preparado: no vuelve a cargar
recursos ni reinicia el menú. Desactivar y reactivar el módulo también es inmediato.
Los demás objetos conservan su búsqueda original.

Se conservan las demás fuentes de recursos del usuario. La descarga de la DLL
restaura primero la función original y retira los recursos en memoria antes de
liberar los callbacks nativos. El ZIP generado por la versión anterior se retira
de la selección y se elimina tras cerrarse.

La configuración tiene dos pestañas: Cosmetics para elegir entre 20 modelos,
y Preview para ver el cosmético seleccionado aun con el módulo desactivado.
Las texturas de la vista previa pertenecen al atlas existente del menú y se
liberan junto con él.

Las pruebas cubren inicio diferido, selección y desactivación, y una JVM real
con las interfaces equivalentes de Minecraft 1.21.11. Verifican los 20 modelos,
la entrega exacta de geometrías/texturas en memoria, las vistas previas,
la conservación de otras fuentes, y la restauración previa a la descarga.
También comprueban que los 20 cambios de espada usan una única recarga inicial
y que desactivar/reactivar no vuelve a recargar recursos.
La apariencia dentro de Minecraft requiere validación en una partida.
