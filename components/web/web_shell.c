#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "web_shell.h"

#if CONFIG_RW_TELNET_ENABLE

#define MAX_FRAME 4096

static const char *TAG = "web_shell";

typedef struct {
    httpd_handle_t hd;
    int fd;
    int sock;
    int refs;
    bool gone;
    SemaphoreHandle_t sent;
    size_t len;
    uint8_t buf[512];
} bridge_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static const char SHELL_PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\"><title>Shell</title><style>"
    "html,body{height:100%;margin:0;background:#111;color:#ddd;font-family:system-ui,sans-serif}"
    "body{display:flex;flex-direction:column}"
    "#t{flex:1;margin:0;padding:8px;overflow-y:auto;font:14px/1.3 ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;word-break:break-all;cursor:text}"
    ".c{background:#ddd;color:#111}"
    "#k{position:fixed;left:0;bottom:0;width:1px;height:1px;opacity:0;border:0;padding:0}"
    "nav{padding:4px 8px;font-size:.85rem;background:#222}a{color:#8af}"
    "</style></head><body><pre id=t></pre>"
    "<nav><a href=/>Status</a> · <span id=s>connecting</span> <button id=b hidden>Reconnect</button></nav>"
    "<textarea id=k autocapitalize=off autocomplete=off autocorrect=off spellcheck=false></textarea><script>"
    "const $=i=>document.getElementById(i),t=$('t'),k=$('k'),enc=new TextEncoder();"
    "const K={Enter:'\\r',Backspace:'\\x7f',Tab:'\\t',Escape:'\\x1b',ArrowUp:'\\x1b[A',ArrowDown:'\\x1b[B',ArrowRight:'\\x1b[C',ArrowLeft:'\\x1b[D',Home:'\\x1b[H',End:'\\x1b[F',Delete:'\\x1b[3~'};"
    "let ws,dec,lines,row,col,esc,iac,cw=8,lh=18;"
    "function reset(){lines=[''];row=0;col=0;esc=null;iac=0;dec=new TextDecoder()}"
    "function put(ch){let l=lines[row];if(l.length<col)l+=' '.repeat(col-l.length);lines[row]=l.slice(0,col)+ch+l.slice(col+1);col++}"
    "function nl(){row++;if(row==lines.length)lines.push('');if(lines.length>1000){lines.shift();row--}}"
    "function csi(s){const c=s.slice(-1),n=parseInt(s.slice(1))||1;"
    "if(c=='K')lines[row]=lines[row].slice(0,col);else if(c=='C')col+=n;else if(c=='D')col=Math.max(0,col-n);"
    "else if(c=='J'){lines=[''];row=0;col=0}}"
    "function text(s){for(const ch of s){"
    "if(esc!==null){esc+=ch;if(esc.length==1&&ch!='[')esc=null;else if(esc.length>1&&ch>='@'&&ch<='~'){csi(esc);esc=null}continue}"
    "if(ch=='\\x1b')esc='';else if(ch=='\\r')col=0;else if(ch=='\\n')nl();else if(ch=='\\b')col=Math.max(0,col-1);else if(ch>=' ')put(ch)}}"
    "function bytes(a){const o=[];for(const b of a){"
    "if(iac==1){iac=b==255?(o.push(255),0):b==250?3:b>=251?2:0;continue}"
    "if(iac==2){iac=0;continue}if(iac==3){if(b==255)iac=4;continue}if(iac==4){iac=b==240?0:3;continue}"
    "if(b==255){iac=1;continue}o.push(b)}"
    "text(dec.decode(new Uint8Array(o),{stream:true}));render()}"
    "function h(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;')}"
    "function render(){const l=lines[row].padEnd(col+1);"
    "t.innerHTML=lines.slice(0,row).map(x=>h(x)+'\\n').join('')+h(l.slice(0,col))+'<span class=c>'+h(l[col])+'</span>'+h(l.slice(col+1))"
    "+lines.slice(row+1).map(x=>'\\n'+h(x)).join('');t.scrollTop=t.scrollHeight}"
    "function send(s){if(ws&&ws.readyState==1)ws.send(enc.encode(s))}"
    "function naws(){const c=Math.max(20,Math.min(250,Math.floor((t.clientWidth-16)/cw))),r=Math.max(5,Math.min(250,Math.floor((t.clientHeight-16)/lh)));"
    "if(ws&&ws.readyState==1)ws.send(new Uint8Array([255,250,31,0,c,0,r,255,240]))}"
    "function measure(){const m=document.createElement('span');m.style.cssText='position:absolute;visibility:hidden;white-space:pre';"
    "m.style.font=getComputedStyle(t).font;m.textContent='M'.repeat(20);document.body.appendChild(m);"
    "const r=m.getBoundingClientRect();cw=r.width/20;lh=r.height;m.remove()}"
    "function connect(){reset();render();$('s').textContent='connecting';$('b').hidden=true;"
    "ws=new WebSocket((location.protocol=='https:'?'wss://':'ws://')+location.host+'/api/shell');ws.binaryType='arraybuffer';"
    "ws.onopen=()=>{$('s').textContent='connected';naws();k.focus()};"
    "ws.onmessage=e=>{if(typeof e.data=='string'){text(e.data);render()}else bytes(new Uint8Array(e.data))};"
    "ws.onclose=()=>{$('s').textContent='closed';$('b').hidden=false}}"
    "k.onkeydown=e=>{let s;"
    "if(e.ctrlKey&&!e.altKey&&!e.metaKey&&e.key.length==1){const c=e.key.toLowerCase();"
    "if(c=='v'||e.shiftKey||(c=='c'&&getSelection().toString()))return;if(c>='a'&&c<='z')s=String.fromCharCode(c.charCodeAt(0)-96)}"
    "else if(K[e.key])s=K[e.key];"
    "if(s!==undefined){e.preventDefault();send(s)}};"
    "k.oninput=()=>{send(k.value.replace(/\\r?\\n/g,'\\r'));k.value=''};"
    "t.onmouseup=()=>{if(!getSelection().toString())k.focus()};"
    "$('b').onclick=connect;addEventListener('resize',naws);measure();connect()"
    "</script></body></html>";

static void release(bridge_t *b)
{
    taskENTER_CRITICAL(&s_lock);
    bool last = --b->refs == 0;
    taskEXIT_CRITICAL(&s_lock);
    if (last) {
        close(b->sock);
        vSemaphoreDelete(b->sent);
        free(b);
    }
}

static void bridge_free(void *ctx)
{
    bridge_t *b = ctx;
    b->gone = true;
    shutdown(b->sock, SHUT_RDWR);
    release(b);
}

static void send_work(void *arg)
{
    bridge_t *b = arg;
    if (!b->gone) {
        httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_BINARY, .payload = b->buf, .len = b->len, .final = true };
        httpd_ws_send_frame_async(b->hd, b->fd, &frame);
    }
    xSemaphoreGive(b->sent);
}

static void close_work(void *arg)
{
    bridge_t *b = arg;
    if (!b->gone) {
        httpd_sess_trigger_close(b->hd, b->fd);
    }
    release(b);
}

static void proxy_task(void *arg)
{
    bridge_t *b = arg;
    for (;;) {
        ssize_t n = recv(b->sock, b->buf, sizeof(b->buf), 0);
        if (n <= 0) {
            break;
        }
        b->len = n;
        if (httpd_queue_work(b->hd, send_work, b) != ESP_OK) {
            break;
        }
        xSemaphoreTake(b->sent, portMAX_DELAY);
    }
    if (httpd_queue_work(b->hd, close_work, b) != ESP_OK) {
        release(b);
    }
    vTaskDelete(NULL);
}

static esp_err_t ws_reply(httpd_req_t *req, const char *msg)
{
    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)msg, .len = strlen(msg), .final = true };
    httpd_ws_send_frame(req, &frame);
    return ESP_FAIL;
}

static esp_err_t open_bridge(httpd_req_t *req)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_RW_TELNET_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (sock < 0 || connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "connect to telnet: errno %d", errno);
        if (sock >= 0) {
            close(sock);
        }
        return ws_reply(req, "shell unavailable: telnet server not running\r\n");
    }
    bridge_t *b = calloc(1, sizeof(*b));
    SemaphoreHandle_t sent = xSemaphoreCreateBinary();
    if (!b || !sent) {
        close(sock);
        free(b);
        if (sent) {
            vSemaphoreDelete(sent);
        }
        return ws_reply(req, "shell unavailable: out of memory\r\n");
    }
    *b = (bridge_t){ .hd = req->handle, .fd = httpd_req_to_sockfd(req), .sock = sock, .refs = 2, .sent = sent };
    if (xTaskCreate(proxy_task, "web_shell", 3072, b, 2, NULL) != pdPASS) {
        b->refs = 1;
        release(b);
        return ws_reply(req, "shell unavailable: out of memory\r\n");
    }
    req->sess_ctx = b;
    req->free_ctx = bridge_free;
    return ESP_OK;
}

static esp_err_t handle_ws(httpd_req_t *req)
{
    httpd_ws_frame_t frame = { 0 };
    ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(req, &frame, 0), TAG, "frame header");
    if (frame.len > MAX_FRAME) {
        return ESP_FAIL;
    }
    if (frame.len) {
        frame.payload = malloc(frame.len);
        if (!frame.payload) {
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = frame.len ? httpd_ws_recv_frame(req, &frame, frame.len) : ESP_OK;
    if (err == ESP_OK && !req->sess_ctx) {
        err = open_bridge(req);
    }
    bridge_t *b = req->sess_ctx;
    if (err == ESP_OK && frame.len && (frame.type == HTTPD_WS_TYPE_BINARY || frame.type == HTTPD_WS_TYPE_TEXT)) {
        send(b->sock, frame.payload, frame.len, 0);
    }
    free(frame.payload);
    return err;
}

static esp_err_t handle_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, SHELL_PAGE, sizeof(SHELL_PAGE) - 1);
}

esp_err_t web_shell_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/shell", .method = HTTP_GET, .handler = handle_page },
        { .uri = "/api/shell", .method = HTTP_GET, .handler = handle_ws, .is_websocket = true },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "register %s", uris[i].uri);
    }
    return ESP_OK;
}

#else

esp_err_t web_shell_register(httpd_handle_t server)
{
    return ESP_OK;
}

#endif
