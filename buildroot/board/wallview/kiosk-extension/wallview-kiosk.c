#include <string.h>
#include <wpe/webkit-web-process-extension.h>

#define SCRIPT_PATH "/usr/share/wallview/kiosk.js"

static gchar *script;

static void
window_object_cleared(WebKitScriptWorld *world, G_GNUC_UNUSED WebKitWebPage *page, WebKitFrame *frame, G_GNUC_UNUSED gpointer user_data)
{
    const gchar *uri;
    JSCContext *context;
    JSCValue *result;

    if (!script || !webkit_frame_is_main_frame(frame))
        return;
    uri = webkit_frame_get_uri(frame);
    if (!uri || (strncmp(uri, "http://", 7) != 0 && strncmp(uri, "https://", 8) != 0))
        return;
    context = webkit_frame_get_js_context_for_script_world(frame, world);
    result = jsc_context_evaluate_with_source_uri(context, script, -1, "wallview-kiosk.js", 1);
    g_clear_object(&result);
    g_object_unref(context);
}

G_MODULE_EXPORT void
webkit_web_process_extension_initialize(G_GNUC_UNUSED WebKitWebProcessExtension *extension)
{
    const gchar *path = g_getenv("WALLVIEW_KIOSK_SCRIPT");

    if (!g_file_get_contents(path ? path : SCRIPT_PATH, &script, NULL, NULL))
        return;
    g_signal_connect(webkit_script_world_get_default(), "window-object-cleared",
                     G_CALLBACK(window_object_cleared), NULL);
}
