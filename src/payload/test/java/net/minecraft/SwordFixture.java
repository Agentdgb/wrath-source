package net.minecraft;
import java.util.*;
import java.util.function.*;
import java.io.*;
import java.util.concurrent.*;

public final class SwordFixture {}
class class_2561 {
    final String value;
    class_2561(String value) {this.value=value;}
    static class_5250 method_43470(String value) { return new class_5250(value); }
}
class class_5250 extends class_2561 { class_5250(String value) {super(value);} }
class class_5352 { static final class_5352 field_25347 = new class_5352(); }
class class_9224 {
    final String id;
    class_9224(String id,class_2561 title,class_5352 source,Optional<?> known) {this.id=id;}
}
class class_3281 { static final class_3281 field_14224 = new class_3281(); }
class class_7699 { static class_7699 method_45397() {return new class_7699();} }
class class_9225 { class_9225(boolean required,class_3288.class_3289 position,boolean pinned) {} }
class class_3264 { static final class_3264 field_14188 = new class_3264(); }
class class_2960 {
    final String namespace,path;
    class_2960(String namespace,String path){this.namespace=namespace;this.path=path;}
    static class_2960 method_60655(String namespace,String path) {return new class_2960(namespace,path);}
    String method_12832(){return path;}
    String method_12836(){return namespace;}
}
interface class_7367 { InputStream get(); }
interface class_3262 extends AutoCloseable {
    class_9224 method_56926();
    String method_14409();
    Optional<?> method_56929();
    Set<String> method_14406(class_3264 type);
    class_7367 method_14405(class_3264 type,class_2960 id);
    class_7367 method_14410(String... path);
    Object method_14407(Object serializer);
    void method_14408(class_3264 type,String namespace,String prefix,class_7664 consumer);
    void close();
    interface class_7664 extends BiConsumer<class_2960,class_7367> {}
}
interface class_3285 { void method_14453(Consumer<class_3288> output); }
class class_3288 {
    final class_9224 info;
    final class_7680 factory;
    class_3288(class_9224 info,class_7680 factory,class_7679 metadata,class_9225 position) {this.info=info;this.factory=factory;}
    interface class_7680 {
        class_3262 method_52424(class_9224 info);
        class_3262 method_52425(class_9224 info,class_7679 metadata);
    }
    static class class_7679 { class_7679(class_2561 text,class_3281 compatibility,class_7699 features,List<?> overlays) {} }
    static class class_3289 { static final class_3289 field_14280 = new class_3289(); }
}
class class_3283 {
    final Set<class_3285> field_14227 = new LinkedHashSet<>();
    final Map<String,class_3288> profiles = new HashMap<>();
    List<String> enabled = new ArrayList<>(List.of("vanilla","file/existing.zip","file/wrath-swordreplace-godweer.zip"));
    void method_14445() {profiles.clear();for(class_3285 p:field_14227)p.method_14453(profile->profiles.put(profile.info.id,profile));}
    Collection<String> method_29210() {return enabled;}
    void method_14447(Collection<String> ids) {enabled=new ArrayList<>(ids);}
    class_3288 method_14449(String id) {return profiles.get(id);}
}
class class_10442 {
    final Function<class_2960,String> field_55334 = key -> "baked:"+key.namespace+":"+key.path;
}
class class_310 {
    static final class_310 INSTANCE = new class_310();
    final class_3283 field_1715 = new class_3283();
    final class_10442 field_55226 = new class_10442();
    CompletableFuture<Void> future;
    class_3262 active;
    int reloads;
    static class_310 method_1551() {return INSTANCE;}
    CompletableFuture<Void> method_1521() {
        ++reloads;
        if(active!=null)active.close();
        class_3288 profile=field_1715.profiles.get("wrath:cosmetics");
        active=field_1715.enabled.contains("wrath:cosmetics") && profile!=null ? profile.factory.method_52424(profile.info) : null;
        future=new CompletableFuture<>();return future;
    }
    static void completeReload() {INSTANCE.future.complete(null);}
    static String cosmeticJson(int index) throws Exception {
        class_7367 source=INSTANCE.active.method_14405(class_3264.field_14188,new class_2960("minecraft","items/wrath_cosmetic/"+index+".json"));
        return new String(source.get().readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
    }
    static String renderSword() { return INSTANCE.field_55226.field_55334.apply(new class_2960("minecraft","diamond_sword")); }
    static String renderOther() { return INSTANCE.field_55226.field_55334.apply(new class_2960("minecraft","apple")); }
    static int countResources() {
        final int[] count={0};
        INSTANCE.active.method_14408(class_3264.field_14188,"minecraft","models/",(location,input)->{if(input.get()!=null)count[0]++;});
        return count[0];
    }
    static byte[] readResource(String path) throws Exception {
        class_7367 input=INSTANCE.active.method_14405(class_3264.field_14188,new class_2960("minecraft",path));
        return input==null ? null : input.get().readAllBytes();
    }
}
