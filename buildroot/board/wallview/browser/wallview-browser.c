#include <glib-unix.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#define RELOAD_DELAY_SECONDS 2

typedef struct {
    GMainLoop *loop;
    WebKitWebView *view;
    gchar *uri;
} Browser;

static gboolean
quit(gpointer user_data)
{
    Browser *browser = user_data;

    g_main_loop_quit(browser->loop);
    return G_SOURCE_REMOVE;
}

static gboolean
reload(gpointer user_data)
{
    Browser *browser = user_data;

    webkit_web_view_load_uri(browser->view, browser->uri);
    return G_SOURCE_REMOVE;
}

static void
web_process_terminated(G_GNUC_UNUSED WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer user_data)
{
    g_warning("Web process terminated (reason %d), reloading", reason);
    g_timeout_add_seconds(RELOAD_DELAY_SECONDS, reload, user_data);
}

static WebKitNetworkSession *
create_network_session(void)
{
    g_autofree gchar *data = g_build_filename(g_get_user_data_dir(), "wpe", NULL);
    g_autofree gchar *cache = g_build_filename(g_get_user_cache_dir(), "wpe", NULL);
    WebKitNetworkSession *session = webkit_network_session_new(data, cache);
    WebKitCookieManager *cookies = webkit_network_session_get_cookie_manager(session);
    g_autofree gchar *cookie_file = g_build_filename(data, "cookies.sqlite", NULL);

    webkit_cookie_manager_set_accept_policy(cookies, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
    webkit_cookie_manager_set_persistent_storage(cookies, cookie_file, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    return session;
}

static WebKitSettings *
create_settings(void)
{
    return webkit_settings_new_with_settings(
        "allow-file-access-from-file-urls", TRUE,
        "allow-universal-access-from-file-urls", TRUE,
        "enable-developer-extras", g_getenv("WALLVIEW_DEVELOPER_EXTRAS") != NULL,
        "enable-page-cache", FALSE,
        NULL);
}

int
main(int argc, char **argv)
{
    Browser browser = { 0 };
    g_autoptr(WebKitWebContext) context = NULL;
    g_autoptr(WebKitNetworkSession) session = NULL;
    g_autoptr(WebKitSettings) settings = NULL;
    WPEView *wpe_view;

    if (argc != 2) {
        g_printerr("usage: %s URI\n", argv[0]);
        return 2;
    }

    browser.uri = argv[1];
    browser.loop = g_main_loop_new(NULL, FALSE);
    context = webkit_web_context_new();
    session = create_network_session();
    settings = create_settings();

    browser.view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "web-context", context,
        "network-session", session,
        "settings", settings,
        NULL));

    wpe_view = webkit_web_view_get_wpe_view(browser.view);
    if (!wpe_view) {
        g_printerr("WPE platform view not available\n");
        return 1;
    }
    wpe_toplevel_fullscreen(wpe_view_get_toplevel(wpe_view));

    g_signal_connect(browser.view, "web-process-terminated", G_CALLBACK(web_process_terminated), &browser);
    g_unix_signal_add(SIGTERM, quit, &browser);
    g_unix_signal_add(SIGINT, quit, &browser);

    webkit_web_view_load_uri(browser.view, browser.uri);
    g_main_loop_run(browser.loop);

    g_object_unref(browser.view);
    g_main_loop_unref(browser.loop);
    return 0;
}
