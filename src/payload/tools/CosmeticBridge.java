package dev.wrath;

// Interface carrier only. All resource operations execute in native C++.
// Embedded in the DLL; never installed as a Java mod or external JAR.
public final class CosmeticBridge implements java.lang.reflect.InvocationHandler {
    public final long key;
    public CosmeticBridge(long key) { this.key = key; }
    @Override public native Object invoke(Object proxy, java.lang.reflect.Method method, Object[] args);
}
