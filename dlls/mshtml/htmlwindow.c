/*
 * Copyright 2006-2010 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "ole2.h"
#include "mshtmdid.h"
#include "shlguid.h"

#define NO_SHLWAPI_REG
#include "shlwapi.h"

#include "wine/debug.h"

#include "mshtml_private.h"
#include "htmlevent.h"
#include "binding.h"
#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(mshtml);

static struct list window_list = LIST_INIT(window_list);

static void window_set_docnode(HTMLWindow *window, HTMLDocumentNode *doc_node)
{
    if(window->doc) {
        if(window->doc_obj && window == window->doc_obj->basedoc.window)
            window->doc->basedoc.cp_container.forward_container = NULL;
        abort_document_bindings(window->doc);
        window->doc->basedoc.window = NULL;
        htmldoc_release(&window->doc->basedoc);
    }
    window->doc = doc_node;
    if(doc_node)
        htmldoc_addref(&doc_node->basedoc);

    if(window->doc_obj && window->doc_obj->basedoc.window == window) {
        if(window->doc_obj->basedoc.doc_node)
            htmldoc_release(&window->doc_obj->basedoc.doc_node->basedoc);
        window->doc_obj->basedoc.doc_node = doc_node;
        if(doc_node)
            htmldoc_addref(&doc_node->basedoc);
    }

    if(doc_node && window->doc_obj && window->doc_obj->usermode == EDITMODE) {
        nsIDOMNSHTMLDocument *nshtmldoc;
        nsAString mode_str;
        nsresult nsres;

        static const PRUnichar onW[] = {'o','n',0};

        nsres = nsIDOMHTMLDocument_QueryInterface(doc_node->nsdoc, &IID_nsIDOMNSHTMLDocument, (void**)&nshtmldoc);
        if(NS_SUCCEEDED(nsres)) {
            nsAString_Init(&mode_str, onW);
            nsres = nsIDOMNSHTMLDocument_SetDesignMode(nshtmldoc, &mode_str);
            nsAString_Finish(&mode_str);
            nsIDOMNSHTMLDocument_Release(nshtmldoc);
            if(NS_FAILED(nsres))
                ERR("SetDesignMode failed: %08x\n", nsres);
        }else {
            ERR("Could not get nsIDOMNSHTMLDocument interface: %08x\n", nsres);
        }
    }
}

static void release_children(HTMLWindow *This)
{
    HTMLWindow *child;

    while(!list_empty(&This->children)) {
        child = LIST_ENTRY(list_tail(&This->children), HTMLWindow, sibling_entry);

        list_remove(&child->sibling_entry);
        child->parent = NULL;
        IHTMLWindow2_Release(&child->IHTMLWindow2_iface);
    }
}

static HRESULT get_location(HTMLWindow *This, HTMLLocation **ret)
{
    if(This->location) {
        IHTMLLocation_AddRef(&This->location->IHTMLLocation_iface);
    }else {
        HRESULT hres;

        hres = HTMLLocation_Create(This, &This->location);
        if(FAILED(hres))
            return hres;
    }

    *ret = This->location;
    return S_OK;
}

static inline HRESULT set_window_event(HTMLWindow *window, eventid_t eid, VARIANT *var)
{
    if(!window->doc) {
        FIXME("No document\n");
        return E_FAIL;
    }

    return set_event_handler(&window->doc->body_event_target, NULL, window->doc, eid, var);
}

static inline HRESULT get_window_event(HTMLWindow *window, eventid_t eid, VARIANT *var)
{
    if(!window->doc) {
        FIXME("No document\n");
        return E_FAIL;
    }

    return get_event_handler(&window->doc->body_event_target, eid, var);
}

static inline HTMLWindow *impl_from_IHTMLWindow2(IHTMLWindow2 *iface)
{
    return CONTAINING_RECORD(iface, HTMLWindow, IHTMLWindow2_iface);
}

static HRESULT WINAPI HTMLWindow2_QueryInterface(IHTMLWindow2 *iface, REFIID riid, void **ppv)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    *ppv = NULL;

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        TRACE("(%p)->(IID_IUnknown %p)\n", This, ppv);
        *ppv = &This->IHTMLWindow2_iface;
    }else if(IsEqualGUID(&IID_IDispatch, riid)) {
        TRACE("(%p)->(IID_IDispatch %p)\n", This, ppv);
        *ppv = &This->IHTMLWindow2_iface;
    }else if(IsEqualGUID(&IID_IDispatchEx, riid)) {
        TRACE("(%p)->(IID_IDispatchEx %p)\n", This, ppv);
        *ppv = &This->IDispatchEx_iface;
    }else if(IsEqualGUID(&IID_IHTMLFramesCollection2, riid)) {
        TRACE("(%p)->(IID_IHTMLFramesCollection2 %p)\n", This, ppv);
        *ppv = &This->IHTMLWindow2_iface;
    }else if(IsEqualGUID(&IID_IHTMLWindow2, riid)) {
        TRACE("(%p)->(IID_IHTMLWindow2 %p)\n", This, ppv);
        *ppv = &This->IHTMLWindow2_iface;
    }else if(IsEqualGUID(&IID_IHTMLWindow3, riid)) {
        TRACE("(%p)->(IID_IHTMLWindow3 %p)\n", This, ppv);
        *ppv = &This->IHTMLWindow3_iface;
    }else if(IsEqualGUID(&IID_IHTMLWindow4, riid)) {
        TRACE("(%p)->(IID_IHTMLWindow4 %p)\n", This, ppv);
        *ppv = &This->IHTMLWindow4_iface;
    }else if(IsEqualGUID(&IID_IHTMLPrivateWindow, riid)) {
        TRACE("(%p)->(IID_IHTMLPrivateWindow %p)\n", This, ppv);
        *ppv = &This->IHTMLPrivateWindow_iface;
    }else if(IsEqualGUID(&IID_IServiceProvider, riid)) {
        TRACE("(%p)->(IID_IServiceProvider %p)\n", This, ppv);
        *ppv = &This->IServiceProvider_iface;
    }else if(dispex_query_interface(&This->dispex, riid, ppv)) {
        return *ppv ? S_OK : E_NOINTERFACE;
    }

    if(*ppv) {
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    WARN("(%p)->(%s %p)\n", This, debugstr_guid(riid), ppv);
    return E_NOINTERFACE;
}

static ULONG WINAPI HTMLWindow2_AddRef(IHTMLWindow2 *iface)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%d\n", This, ref);

    return ref;
}

static ULONG WINAPI HTMLWindow2_Release(IHTMLWindow2 *iface)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%d\n", This, ref);

    if(!ref) {
        DWORD i;

        remove_target_tasks(This->task_magic);
        set_window_bscallback(This, NULL);
        set_current_mon(This, NULL);
        window_set_docnode(This, NULL);
        release_children(This);

        if(This->secmgr)
            IInternetSecurityManager_Release(This->secmgr);

        if(This->frame_element)
            This->frame_element->content_window = NULL;

        if(This->option_factory) {
            This->option_factory->window = NULL;
            IHTMLOptionElementFactory_Release(&This->option_factory->IHTMLOptionElementFactory_iface);
        }

        if(This->image_factory) {
            This->image_factory->window = NULL;
            IHTMLImageElementFactory_Release(&This->image_factory->IHTMLImageElementFactory_iface);
        }

        if(This->location) {
            This->location->window = NULL;
            IHTMLLocation_Release(&This->location->IHTMLLocation_iface);
        }

        if(This->screen)
            IHTMLScreen_Release(This->screen);

        for(i=0; i < This->global_prop_cnt; i++)
            heap_free(This->global_props[i].name);

        This->window_ref->window = NULL;
        windowref_release(This->window_ref);

        heap_free(This->global_props);
        release_script_hosts(This);

        if(This->nswindow)
            nsIDOMWindow_Release(This->nswindow);

        list_remove(&This->entry);
        release_dispex(&This->dispex);
        heap_free(This);
    }

    return ref;
}

static HRESULT WINAPI HTMLWindow2_GetTypeInfoCount(IHTMLWindow2 *iface, UINT *pctinfo)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    return IDispatchEx_GetTypeInfoCount(&This->IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI HTMLWindow2_GetTypeInfo(IHTMLWindow2 *iface, UINT iTInfo,
                                              LCID lcid, ITypeInfo **ppTInfo)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    return IDispatchEx_GetTypeInfo(&This->IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI HTMLWindow2_GetIDsOfNames(IHTMLWindow2 *iface, REFIID riid,
                                                LPOLESTR *rgszNames, UINT cNames,
                                                LCID lcid, DISPID *rgDispId)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    return IDispatchEx_GetIDsOfNames(&This->IDispatchEx_iface, riid, rgszNames, cNames, lcid,
            rgDispId);
}

static HRESULT WINAPI HTMLWindow2_Invoke(IHTMLWindow2 *iface, DISPID dispIdMember,
                            REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
                            VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    return IDispatchEx_Invoke(&This->IDispatchEx_iface, dispIdMember, riid, lcid, wFlags,
            pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT get_frame_by_index(nsIDOMWindowCollection *nsFrames, PRUint32 index, HTMLWindow **ret)
{
    PRUint32 length;
    nsIDOMWindow *nsWindow;
    nsresult nsres;

    nsres = nsIDOMWindowCollection_GetLength(nsFrames, &length);
    if(NS_FAILED(nsres)) {
        FIXME("nsIDOMWindowCollection_GetLength failed: 0x%08x\n", nsres);
        return E_FAIL;
    }

    if(index >= length)
        return DISP_E_MEMBERNOTFOUND;

    nsres = nsIDOMWindowCollection_Item(nsFrames, index, &nsWindow);
    if(NS_FAILED(nsres)) {
        FIXME("nsIDOMWindowCollection_Item failed: 0x%08x\n", nsres);
        return E_FAIL;
    }

    *ret = nswindow_to_window(nsWindow);

    nsIDOMWindow_Release(nsWindow);

    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_item(IHTMLWindow2 *iface, VARIANT *pvarIndex, VARIANT *pvarResult)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    nsIDOMWindowCollection *nsFrames;
    HTMLWindow *window;
    HRESULT hres;
    nsresult nsres;

    TRACE("(%p)->(%p %p)\n", This, pvarIndex, pvarResult);

    nsres = nsIDOMWindow_GetFrames(This->nswindow, &nsFrames);
    if(NS_FAILED(nsres)) {
        FIXME("nsIDOMWindow_GetFrames failed: 0x%08x\n", nsres);
        return E_FAIL;
    }

    if(V_VT(pvarIndex) == VT_I4) {
        int index = V_I4(pvarIndex);
        TRACE("Getting index %d\n", index);
        if(index < 0) {
            hres = DISP_E_MEMBERNOTFOUND;
            goto cleanup;
        }
        hres = get_frame_by_index(nsFrames, index, &window);
        if(FAILED(hres))
            goto cleanup;
    }else if(V_VT(pvarIndex) == VT_UINT) {
        unsigned int index = V_UINT(pvarIndex);
        TRACE("Getting index %u\n", index);
        hres = get_frame_by_index(nsFrames, index, &window);
        if(FAILED(hres))
            goto cleanup;
    }else if(V_VT(pvarIndex) == VT_BSTR) {
        BSTR str = V_BSTR(pvarIndex);
        PRUint32 length, i;

        TRACE("Getting name %s\n", wine_dbgstr_w(str));

        nsres = nsIDOMWindowCollection_GetLength(nsFrames, &length);

        window = NULL;
        for(i = 0; i < length && !window; ++i) {
            HTMLWindow *cur_window;
            nsIDOMWindow *nsWindow;
            BSTR id;

            nsres = nsIDOMWindowCollection_Item(nsFrames, i, &nsWindow);
            if(NS_FAILED(nsres)) {
                FIXME("nsIDOMWindowCollection_Item failed: 0x%08x\n", nsres);
                hres = E_FAIL;
                goto cleanup;
            }

            cur_window = nswindow_to_window(nsWindow);

            nsIDOMWindow_Release(nsWindow);

            hres = IHTMLElement_get_id(&cur_window->frame_element->element.IHTMLElement_iface, &id);
            if(FAILED(hres)) {
                FIXME("IHTMLElement_get_id failed: 0x%08x\n", hres);
                goto cleanup;
            }

            if(!strcmpW(id, str))
                window = cur_window;

            SysFreeString(id);
        }

        if(!window) {
            hres = DISP_E_MEMBERNOTFOUND;
            goto cleanup;
        }
    }else {
        hres = E_INVALIDARG;
        goto cleanup;
    }

    IHTMLWindow2_AddRef(&window->IHTMLWindow2_iface);
    V_VT(pvarResult) = VT_DISPATCH;
    V_DISPATCH(pvarResult) = (IDispatch*)window;

    hres = S_OK;

cleanup:
    nsIDOMWindowCollection_Release(nsFrames);

    return hres;
}

static HRESULT WINAPI HTMLWindow2_get_length(IHTMLWindow2 *iface, LONG *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    nsIDOMWindowCollection *nscollection;
    PRUint32 length;
    nsresult nsres;

    TRACE("(%p)->(%p)\n", This, p);

    nsres = nsIDOMWindow_GetFrames(This->nswindow, &nscollection);
    if(NS_FAILED(nsres)) {
        ERR("GetFrames failed: %08x\n", nsres);
        return E_FAIL;
    }

    nsres = nsIDOMWindowCollection_GetLength(nscollection, &length);
    nsIDOMWindowCollection_Release(nscollection);
    if(NS_FAILED(nsres)) {
        ERR("GetLength failed: %08x\n", nsres);
        return E_FAIL;
    }

    *p = length;
    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_get_frames(IHTMLWindow2 *iface, IHTMLFramesCollection2 **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p): semi-stub\n", This, p);

    /* FIXME: Should return a separate Window object */
    *p = (IHTMLFramesCollection2*)&This->IHTMLWindow2_iface;
    HTMLWindow2_AddRef(iface);
    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_put_defaultStatus(IHTMLWindow2 *iface, BSTR v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%s)\n", This, debugstr_w(v));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_defaultStatus(IHTMLWindow2 *iface, BSTR *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_put_status(IHTMLWindow2 *iface, BSTR v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    WARN("(%p)->(%s)\n", This, debugstr_w(v));

    /*
     * FIXME: Since IE7, setting status is blocked, but still possible in certain circumstances.
     * Ignoring the call should be enough for us.
     */
    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_get_status(IHTMLWindow2 *iface, BSTR *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    /* See put_status */
    *p = NULL;
    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_setTimeout(IHTMLWindow2 *iface, BSTR expression,
        LONG msec, VARIANT *language, LONG *timerID)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    VARIANT expr_var;

    TRACE("(%p)->(%s %d %p %p)\n", This, debugstr_w(expression), msec, language, timerID);

    V_VT(&expr_var) = VT_BSTR;
    V_BSTR(&expr_var) = expression;

    return IHTMLWindow3_setTimeout(&This->IHTMLWindow3_iface, &expr_var, msec, language, timerID);
}

static HRESULT WINAPI HTMLWindow2_clearTimeout(IHTMLWindow2 *iface, LONG timerID)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%d)\n", This, timerID);

    return clear_task_timer(&This->doc->basedoc, FALSE, timerID);
}

#define MAX_MESSAGE_LEN 2000

static HRESULT WINAPI HTMLWindow2_alert(IHTMLWindow2 *iface, BSTR message)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    WCHAR title[100], *msg = message;
    DWORD len;

    TRACE("(%p)->(%s)\n", This, debugstr_w(message));

    if(!LoadStringW(get_shdoclc(), IDS_MESSAGE_BOX_TITLE, title,
                    sizeof(title)/sizeof(WCHAR))) {
        WARN("Could not load message box title: %d\n", GetLastError());
        return S_OK;
    }

    len = SysStringLen(message);
    if(len > MAX_MESSAGE_LEN) {
        msg = heap_alloc((MAX_MESSAGE_LEN+1)*sizeof(WCHAR));
        if(!msg)
            return E_OUTOFMEMORY;
        memcpy(msg, message, MAX_MESSAGE_LEN*sizeof(WCHAR));
        msg[MAX_MESSAGE_LEN] = 0;
    }

    MessageBoxW(This->doc_obj->hwnd, msg, title, MB_ICONWARNING);
    if(msg != message)
        heap_free(msg);
    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_confirm(IHTMLWindow2 *iface, BSTR message,
        VARIANT_BOOL *confirmed)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    WCHAR wszTitle[100];

    TRACE("(%p)->(%s %p)\n", This, debugstr_w(message), confirmed);

    if(!confirmed) return E_INVALIDARG;

    if(!LoadStringW(get_shdoclc(), IDS_MESSAGE_BOX_TITLE, wszTitle,
                sizeof(wszTitle)/sizeof(WCHAR))) {
        WARN("Could not load message box title: %d\n", GetLastError());
        *confirmed = VARIANT_TRUE;
        return S_OK;
    }

    if(MessageBoxW(This->doc_obj->hwnd, message, wszTitle,
                MB_OKCANCEL|MB_ICONQUESTION)==IDOK)
        *confirmed = VARIANT_TRUE;
    else *confirmed = VARIANT_FALSE;

    return S_OK;
}

typedef struct
{
    BSTR message;
    BSTR dststr;
    VARIANT *textdata;
}prompt_arg;

static INT_PTR CALLBACK prompt_dlgproc(HWND hwnd, UINT msg,
        WPARAM wparam, LPARAM lparam)
{
    switch(msg)
    {
        case WM_INITDIALOG:
        {
            prompt_arg *arg = (prompt_arg*)lparam;
            WCHAR wszTitle[100];

            if(!LoadStringW(get_shdoclc(), IDS_MESSAGE_BOX_TITLE, wszTitle,
                        sizeof(wszTitle)/sizeof(WCHAR))) {
                WARN("Could not load message box title: %d\n", GetLastError());
                EndDialog(hwnd, wparam);
                return FALSE;
            }

            SetWindowLongPtrW(hwnd, DWLP_USER, lparam);
            SetWindowTextW(hwnd, wszTitle);
            SetWindowTextW(GetDlgItem(hwnd, ID_PROMPT_PROMPT), arg->message);
            SetWindowTextW(GetDlgItem(hwnd, ID_PROMPT_EDIT), arg->dststr);
            return FALSE;
        }
        case WM_COMMAND:
            switch(wparam)
            {
                case MAKEWPARAM(IDCANCEL, BN_CLICKED):
                    EndDialog(hwnd, wparam);
                    return TRUE;
                case MAKEWPARAM(IDOK, BN_CLICKED):
                {
                    prompt_arg *arg =
                        (prompt_arg*)GetWindowLongPtrW(hwnd, DWLP_USER);
                    HWND hwndPrompt = GetDlgItem(hwnd, ID_PROMPT_EDIT);
                    INT len = GetWindowTextLengthW(hwndPrompt);

                    if(!arg->textdata)
                    {
                        EndDialog(hwnd, wparam);
                        return TRUE;
                    }

                    V_VT(arg->textdata) = VT_BSTR;
                    if(!len && !arg->dststr)
                        V_BSTR(arg->textdata) = NULL;
                    else
                    {
                        V_BSTR(arg->textdata) = SysAllocStringLen(NULL, len);
                        GetWindowTextW(hwndPrompt, V_BSTR(arg->textdata), len+1);
                    }
                    EndDialog(hwnd, wparam);
                    return TRUE;
                }
            }
            return FALSE;
        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
    }
}

static HRESULT WINAPI HTMLWindow2_prompt(IHTMLWindow2 *iface, BSTR message,
        BSTR dststr, VARIANT *textdata)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    prompt_arg arg;

    TRACE("(%p)->(%s %s %p)\n", This, debugstr_w(message), debugstr_w(dststr), textdata);

    if(textdata) V_VT(textdata) = VT_NULL;

    arg.message = message;
    arg.dststr = dststr;
    arg.textdata = textdata;

    DialogBoxParamW(hInst, MAKEINTRESOURCEW(ID_PROMPT_DIALOG),
            This->doc_obj->hwnd, prompt_dlgproc, (LPARAM)&arg);
    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_get_Image(IHTMLWindow2 *iface, IHTMLImageElementFactory **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    if(!This->image_factory)
        This->image_factory = HTMLImageElementFactory_Create(This);

    *p = &This->image_factory->IHTMLImageElementFactory_iface;
    IHTMLImageElementFactory_AddRef(*p);

    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_get_location(IHTMLWindow2 *iface, IHTMLLocation **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    HTMLLocation *location;
    HRESULT hres;

    TRACE("(%p)->(%p)\n", This, p);

    hres = get_location(This, &location);
    if(FAILED(hres))
        return hres;

    *p = &location->IHTMLLocation_iface;
    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_get_history(IHTMLWindow2 *iface, IOmHistory **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_close(IHTMLWindow2 *iface)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->()\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_put_opener(IHTMLWindow2 *iface, VARIANT v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(v(%d))\n", This, V_VT(&v));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_opener(IHTMLWindow2 *iface, VARIANT *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_navigator(IHTMLWindow2 *iface, IOmNavigator **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    *p = OmNavigator_Create();
    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_put_name(IHTMLWindow2 *iface, BSTR v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    nsAString name_str;
    nsresult nsres;

    TRACE("(%p)->(%s)\n", This, debugstr_w(v));

    nsAString_InitDepend(&name_str, v);
    nsres = nsIDOMWindow_SetName(This->nswindow, &name_str);
    nsAString_Finish(&name_str);
    if(NS_FAILED(nsres))
        ERR("SetName failed: %08x\n", nsres);

    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_get_name(IHTMLWindow2 *iface, BSTR *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    nsAString name_str;
    nsresult nsres;
    HRESULT hres;

    TRACE("(%p)->(%p)\n", This, p);

    nsAString_Init(&name_str, NULL);
    nsres = nsIDOMWindow_GetName(This->nswindow, &name_str);
    if(NS_SUCCEEDED(nsres)) {
        const PRUnichar *name;

        nsAString_GetData(&name_str, &name);
        if(*name) {
            *p = SysAllocString(name);
            hres = *p ? S_OK : E_OUTOFMEMORY;
        }else {
            *p = NULL;
            hres = S_OK;
        }
    }else {
        ERR("GetName failed: %08x\n", nsres);
        hres = E_FAIL;
    }
    nsAString_Finish(&name_str);

    return hres;
}

static HRESULT WINAPI HTMLWindow2_get_parent(IHTMLWindow2 *iface, IHTMLWindow2 **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    TRACE("(%p)->(%p)\n", This, p);

    if(This->parent) {
        *p = &This->parent->IHTMLWindow2_iface;
        IHTMLWindow2_AddRef(*p);
    }else
        *p = NULL;

    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_open(IHTMLWindow2 *iface, BSTR url, BSTR name,
         BSTR features, VARIANT_BOOL replace, IHTMLWindow2 **pomWindowResult)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%s %s %s %x %p)\n", This, debugstr_w(url), debugstr_w(name),
          debugstr_w(features), replace, pomWindowResult);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_self(IHTMLWindow2 *iface, IHTMLWindow2 **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    /* FIXME: We should return kind of proxy window here. */
    IHTMLWindow2_AddRef(&This->IHTMLWindow2_iface);
    *p = &This->IHTMLWindow2_iface;
    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_get_top(IHTMLWindow2 *iface, IHTMLWindow2 **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    HTMLWindow *curr;
    TRACE("(%p)->(%p)\n", This, p);

    curr = This;
    while(curr->parent)
        curr = curr->parent;
    *p = &curr->IHTMLWindow2_iface;
    IHTMLWindow2_AddRef(*p);

    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_get_window(IHTMLWindow2 *iface, IHTMLWindow2 **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    /* FIXME: We should return kind of proxy window here. */
    IHTMLWindow2_AddRef(&This->IHTMLWindow2_iface);
    *p = &This->IHTMLWindow2_iface;
    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_navigate(IHTMLWindow2 *iface, BSTR url)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%s)\n", This, debugstr_w(url));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_put_onfocus(IHTMLWindow2 *iface, VARIANT v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(v(%d))\n", This, V_VT(&v));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_onfocus(IHTMLWindow2 *iface, VARIANT *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_put_onblur(IHTMLWindow2 *iface, VARIANT v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(v(%d))\n", This, V_VT(&v));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_onblur(IHTMLWindow2 *iface, VARIANT *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_put_onload(IHTMLWindow2 *iface, VARIANT v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_window_event(This, EVENTID_LOAD, &v);
}

static HRESULT WINAPI HTMLWindow2_get_onload(IHTMLWindow2 *iface, VARIANT *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_window_event(This, EVENTID_LOAD, p);
}

static HRESULT WINAPI HTMLWindow2_put_onbeforeunload(IHTMLWindow2 *iface, VARIANT v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(v(%d))\n", This, V_VT(&v));

    return set_window_event(This, EVENTID_BEFOREUNLOAD, &v);
}

static HRESULT WINAPI HTMLWindow2_get_onbeforeunload(IHTMLWindow2 *iface, VARIANT *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_window_event(This, EVENTID_BEFOREUNLOAD, p);
}

static HRESULT WINAPI HTMLWindow2_put_onunload(IHTMLWindow2 *iface, VARIANT v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(v(%d))\n", This, V_VT(&v));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_onunload(IHTMLWindow2 *iface, VARIANT *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_put_onhelp(IHTMLWindow2 *iface, VARIANT v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(v(%d))\n", This, V_VT(&v));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_onhelp(IHTMLWindow2 *iface, VARIANT *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_put_onerror(IHTMLWindow2 *iface, VARIANT v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(v(%d))\n", This, V_VT(&v));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_onerror(IHTMLWindow2 *iface, VARIANT *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_put_onresize(IHTMLWindow2 *iface, VARIANT v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    return set_window_event(This, EVENTID_RESIZE, &v);
}

static HRESULT WINAPI HTMLWindow2_get_onresize(IHTMLWindow2 *iface, VARIANT *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    return get_window_event(This, EVENTID_RESIZE, p);
}

static HRESULT WINAPI HTMLWindow2_put_onscroll(IHTMLWindow2 *iface, VARIANT v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(v(%d))\n", This, V_VT(&v));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_onscroll(IHTMLWindow2 *iface, VARIANT *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_document(IHTMLWindow2 *iface, IHTMLDocument2 **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    if(This->doc) {
        /* FIXME: We should return a wrapper object here */
        *p = &This->doc->basedoc.IHTMLDocument2_iface;
        IHTMLDocument2_AddRef(*p);
    }else {
        *p = NULL;
    }

    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_get_event(IHTMLWindow2 *iface, IHTMLEventObj **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    if(This->event)
        IHTMLEventObj_AddRef(This->event);
    *p = This->event;
    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_get__newEnum(IHTMLWindow2 *iface, IUnknown **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_showModalDialog(IHTMLWindow2 *iface, BSTR dialog,
        VARIANT *varArgIn, VARIANT *varOptions, VARIANT *varArgOut)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%s %p %p %p)\n", This, debugstr_w(dialog), varArgIn, varOptions, varArgOut);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_showHelp(IHTMLWindow2 *iface, BSTR helpURL, VARIANT helpArg,
        BSTR features)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%s v(%d) %s)\n", This, debugstr_w(helpURL), V_VT(&helpArg), debugstr_w(features));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_screen(IHTMLWindow2 *iface, IHTMLScreen **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    if(!This->screen) {
        HRESULT hres;

        hres = HTMLScreen_Create(&This->screen);
        if(FAILED(hres))
            return hres;
    }

    *p = This->screen;
    IHTMLScreen_AddRef(This->screen);
    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_get_Option(IHTMLWindow2 *iface, IHTMLOptionElementFactory **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    if(!This->option_factory)
        This->option_factory = HTMLOptionElementFactory_Create(This);

    *p = &This->option_factory->IHTMLOptionElementFactory_iface;
    IHTMLOptionElementFactory_AddRef(*p);

    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_focus(IHTMLWindow2 *iface)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->()\n", This);

    if(This->doc_obj)
        SetFocus(This->doc_obj->hwnd);
    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_get_closed(IHTMLWindow2 *iface, VARIANT_BOOL *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_blur(IHTMLWindow2 *iface)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->()\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_scroll(IHTMLWindow2 *iface, LONG x, LONG y)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%d %d)\n", This, x, y);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_clientInformation(IHTMLWindow2 *iface, IOmNavigator **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_setInterval(IHTMLWindow2 *iface, BSTR expression,
        LONG msec, VARIANT *language, LONG *timerID)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    VARIANT expr;

    TRACE("(%p)->(%s %d %p %p)\n", This, debugstr_w(expression), msec, language, timerID);

    V_VT(&expr) = VT_BSTR;
    V_BSTR(&expr) = expression;
    return IHTMLWindow3_setInterval(&This->IHTMLWindow3_iface, &expr, msec, language, timerID);
}

static HRESULT WINAPI HTMLWindow2_clearInterval(IHTMLWindow2 *iface, LONG timerID)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%d)\n", This, timerID);

    return clear_task_timer(&This->doc->basedoc, TRUE, timerID);
}

static HRESULT WINAPI HTMLWindow2_put_offscreenBuffering(IHTMLWindow2 *iface, VARIANT v)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(v(%d))\n", This, V_VT(&v));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_offscreenBuffering(IHTMLWindow2 *iface, VARIANT *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_execScript(IHTMLWindow2 *iface, BSTR scode, BSTR language,
        VARIANT *pvarRet)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%s %s %p)\n", This, debugstr_w(scode), debugstr_w(language), pvarRet);

    return exec_script(This, scode, language, pvarRet);
}

static HRESULT WINAPI HTMLWindow2_toString(IHTMLWindow2 *iface, BSTR *String)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    static const WCHAR objectW[] = {'[','o','b','j','e','c','t',']',0};

    TRACE("(%p)->(%p)\n", This, String);

    if(!String)
        return E_INVALIDARG;

    *String = SysAllocString(objectW);
    return *String ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI HTMLWindow2_scrollBy(IHTMLWindow2 *iface, LONG x, LONG y)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    nsresult nsres;

    TRACE("(%p)->(%d %d)\n", This, x, y);

    nsres = nsIDOMWindow_ScrollBy(This->nswindow, x, y);
    if(NS_FAILED(nsres))
        ERR("ScrollBy failed: %08x\n", nsres);

    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_scrollTo(IHTMLWindow2 *iface, LONG x, LONG y)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    nsresult nsres;

    TRACE("(%p)->(%d %d)\n", This, x, y);

    nsres = nsIDOMWindow_ScrollTo(This->nswindow, x, y);
    if(NS_FAILED(nsres))
        ERR("ScrollTo failed: %08x\n", nsres);

    return S_OK;
}

static HRESULT WINAPI HTMLWindow2_moveTo(IHTMLWindow2 *iface, LONG x, LONG y)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%d %d)\n", This, x, y);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_moveBy(IHTMLWindow2 *iface, LONG x, LONG y)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%d %d)\n", This, x, y);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_resizeTo(IHTMLWindow2 *iface, LONG x, LONG y)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%d %d)\n", This, x, y);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_resizeBy(IHTMLWindow2 *iface, LONG x, LONG y)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);
    FIXME("(%p)->(%d %d)\n", This, x, y);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow2_get_external(IHTMLWindow2 *iface, IDispatch **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    *p = NULL;

    if(!This->doc_obj->hostui)
        return S_OK;

    return IDocHostUIHandler_GetExternal(This->doc_obj->hostui, p);
}

static const IHTMLWindow2Vtbl HTMLWindow2Vtbl = {
    HTMLWindow2_QueryInterface,
    HTMLWindow2_AddRef,
    HTMLWindow2_Release,
    HTMLWindow2_GetTypeInfoCount,
    HTMLWindow2_GetTypeInfo,
    HTMLWindow2_GetIDsOfNames,
    HTMLWindow2_Invoke,
    HTMLWindow2_item,
    HTMLWindow2_get_length,
    HTMLWindow2_get_frames,
    HTMLWindow2_put_defaultStatus,
    HTMLWindow2_get_defaultStatus,
    HTMLWindow2_put_status,
    HTMLWindow2_get_status,
    HTMLWindow2_setTimeout,
    HTMLWindow2_clearTimeout,
    HTMLWindow2_alert,
    HTMLWindow2_confirm,
    HTMLWindow2_prompt,
    HTMLWindow2_get_Image,
    HTMLWindow2_get_location,
    HTMLWindow2_get_history,
    HTMLWindow2_close,
    HTMLWindow2_put_opener,
    HTMLWindow2_get_opener,
    HTMLWindow2_get_navigator,
    HTMLWindow2_put_name,
    HTMLWindow2_get_name,
    HTMLWindow2_get_parent,
    HTMLWindow2_open,
    HTMLWindow2_get_self,
    HTMLWindow2_get_top,
    HTMLWindow2_get_window,
    HTMLWindow2_navigate,
    HTMLWindow2_put_onfocus,
    HTMLWindow2_get_onfocus,
    HTMLWindow2_put_onblur,
    HTMLWindow2_get_onblur,
    HTMLWindow2_put_onload,
    HTMLWindow2_get_onload,
    HTMLWindow2_put_onbeforeunload,
    HTMLWindow2_get_onbeforeunload,
    HTMLWindow2_put_onunload,
    HTMLWindow2_get_onunload,
    HTMLWindow2_put_onhelp,
    HTMLWindow2_get_onhelp,
    HTMLWindow2_put_onerror,
    HTMLWindow2_get_onerror,
    HTMLWindow2_put_onresize,
    HTMLWindow2_get_onresize,
    HTMLWindow2_put_onscroll,
    HTMLWindow2_get_onscroll,
    HTMLWindow2_get_document,
    HTMLWindow2_get_event,
    HTMLWindow2_get__newEnum,
    HTMLWindow2_showModalDialog,
    HTMLWindow2_showHelp,
    HTMLWindow2_get_screen,
    HTMLWindow2_get_Option,
    HTMLWindow2_focus,
    HTMLWindow2_get_closed,
    HTMLWindow2_blur,
    HTMLWindow2_scroll,
    HTMLWindow2_get_clientInformation,
    HTMLWindow2_setInterval,
    HTMLWindow2_clearInterval,
    HTMLWindow2_put_offscreenBuffering,
    HTMLWindow2_get_offscreenBuffering,
    HTMLWindow2_execScript,
    HTMLWindow2_toString,
    HTMLWindow2_scrollBy,
    HTMLWindow2_scrollTo,
    HTMLWindow2_moveTo,
    HTMLWindow2_moveBy,
    HTMLWindow2_resizeTo,
    HTMLWindow2_resizeBy,
    HTMLWindow2_get_external
};

static inline HTMLWindow *impl_from_IHTMLWindow3(IHTMLWindow3 *iface)
{
    return CONTAINING_RECORD(iface, HTMLWindow, IHTMLWindow3_iface);
}

static HRESULT WINAPI HTMLWindow3_QueryInterface(IHTMLWindow3 *iface, REFIID riid, void **ppv)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);

    return IHTMLWindow2_QueryInterface(&This->IHTMLWindow2_iface, riid, ppv);
}

static ULONG WINAPI HTMLWindow3_AddRef(IHTMLWindow3 *iface)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);

    return IHTMLWindow2_AddRef(&This->IHTMLWindow2_iface);
}

static ULONG WINAPI HTMLWindow3_Release(IHTMLWindow3 *iface)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);

    return IHTMLWindow2_Release(&This->IHTMLWindow2_iface);
}

static HRESULT WINAPI HTMLWindow3_GetTypeInfoCount(IHTMLWindow3 *iface, UINT *pctinfo)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);

    return IDispatchEx_GetTypeInfoCount(&This->IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI HTMLWindow3_GetTypeInfo(IHTMLWindow3 *iface, UINT iTInfo,
                                              LCID lcid, ITypeInfo **ppTInfo)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);

    return IDispatchEx_GetTypeInfo(&This->IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI HTMLWindow3_GetIDsOfNames(IHTMLWindow3 *iface, REFIID riid,
                                                LPOLESTR *rgszNames, UINT cNames,
                                                LCID lcid, DISPID *rgDispId)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);

    return IDispatchEx_GetIDsOfNames(&This->IDispatchEx_iface, riid, rgszNames, cNames, lcid,
            rgDispId);
}

static HRESULT WINAPI HTMLWindow3_Invoke(IHTMLWindow3 *iface, DISPID dispIdMember,
                            REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
                            VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);

    return IDispatchEx_Invoke(&This->IDispatchEx_iface, dispIdMember, riid, lcid, wFlags,
            pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI HTMLWindow3_get_screenLeft(IHTMLWindow3 *iface, LONG *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow3_get_screenTop(IHTMLWindow3 *iface, LONG *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow3_attachEvent(IHTMLWindow3 *iface, BSTR event, IDispatch *pDisp, VARIANT_BOOL *pfResult)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);

    TRACE("(%p)->(%s %p %p)\n", This, debugstr_w(event), pDisp, pfResult);

    if(!This->doc) {
        FIXME("No document\n");
        return E_FAIL;
    }

    return attach_event(&This->doc->body_event_target, NULL, &This->doc->basedoc, event, pDisp, pfResult);
}

static HRESULT WINAPI HTMLWindow3_detachEvent(IHTMLWindow3 *iface, BSTR event, IDispatch *pDisp)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);
    FIXME("(%p)->()\n", This);
    return E_NOTIMPL;
}

static HRESULT window_set_timer(HTMLWindow *This, VARIANT *expr, LONG msec, VARIANT *language,
        BOOL interval, LONG *timer_id)
{
    IDispatch *disp = NULL;

    switch(V_VT(expr)) {
    case VT_DISPATCH:
        disp = V_DISPATCH(expr);
        IDispatch_AddRef(disp);
        break;

    case VT_BSTR:
        disp = script_parse_event(This, V_BSTR(expr));
        break;

    default:
        FIXME("unimplemented vt=%d\n", V_VT(expr));
        return E_NOTIMPL;
    }

    if(!disp)
        return E_FAIL;

    *timer_id = set_task_timer(&This->doc->basedoc, msec, interval, disp);
    IDispatch_Release(disp);

    return S_OK;
}

static HRESULT WINAPI HTMLWindow3_setTimeout(IHTMLWindow3 *iface, VARIANT *expression, LONG msec,
        VARIANT *language, LONG *timerID)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);

    TRACE("(%p)->(%p(%d) %d %p %p)\n", This, expression, V_VT(expression), msec, language, timerID);

    return window_set_timer(This, expression, msec, language, FALSE, timerID);
}

static HRESULT WINAPI HTMLWindow3_setInterval(IHTMLWindow3 *iface, VARIANT *expression, LONG msec,
        VARIANT *language, LONG *timerID)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);

    TRACE("(%p)->(%p %d %p %p)\n", This, expression, msec, language, timerID);

    return window_set_timer(This, expression, msec, language, TRUE, timerID);
}

static HRESULT WINAPI HTMLWindow3_print(IHTMLWindow3 *iface)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);
    FIXME("(%p)\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow3_put_onbeforeprint(IHTMLWindow3 *iface, VARIANT v)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);
    FIXME("(%p)->()\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow3_get_onbeforeprint(IHTMLWindow3 *iface, VARIANT *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow3_put_onafterprint(IHTMLWindow3 *iface, VARIANT v)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);
    FIXME("(%p)->()\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow3_get_onafterprint(IHTMLWindow3 *iface, VARIANT *p)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow3_get_clipboardData(IHTMLWindow3 *iface, IHTMLDataTransfer **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow3_showModelessDialog(IHTMLWindow3 *iface, BSTR url,
        VARIANT *varArgIn, VARIANT *options, IHTMLWindow2 **pDialog)
{
    HTMLWindow *This = impl_from_IHTMLWindow3(iface);
    FIXME("(%p)->(%s %p %p %p)\n", This, debugstr_w(url), varArgIn, options, pDialog);
    return E_NOTIMPL;
}

static const IHTMLWindow3Vtbl HTMLWindow3Vtbl = {
    HTMLWindow3_QueryInterface,
    HTMLWindow3_AddRef,
    HTMLWindow3_Release,
    HTMLWindow3_GetTypeInfoCount,
    HTMLWindow3_GetTypeInfo,
    HTMLWindow3_GetIDsOfNames,
    HTMLWindow3_Invoke,
    HTMLWindow3_get_screenLeft,
    HTMLWindow3_get_screenTop,
    HTMLWindow3_attachEvent,
    HTMLWindow3_detachEvent,
    HTMLWindow3_setTimeout,
    HTMLWindow3_setInterval,
    HTMLWindow3_print,
    HTMLWindow3_put_onbeforeprint,
    HTMLWindow3_get_onbeforeprint,
    HTMLWindow3_put_onafterprint,
    HTMLWindow3_get_onafterprint,
    HTMLWindow3_get_clipboardData,
    HTMLWindow3_showModelessDialog
};

static inline HTMLWindow *impl_from_IHTMLWindow4(IHTMLWindow4 *iface)
{
    return CONTAINING_RECORD(iface, HTMLWindow, IHTMLWindow4_iface);
}

static HRESULT WINAPI HTMLWindow4_QueryInterface(IHTMLWindow4 *iface, REFIID riid, void **ppv)
{
    HTMLWindow *This = impl_from_IHTMLWindow4(iface);

    return IHTMLWindow2_QueryInterface(&This->IHTMLWindow2_iface, riid, ppv);
}

static ULONG WINAPI HTMLWindow4_AddRef(IHTMLWindow4 *iface)
{
    HTMLWindow *This = impl_from_IHTMLWindow4(iface);

    return IHTMLWindow2_AddRef(&This->IHTMLWindow2_iface);
}

static ULONG WINAPI HTMLWindow4_Release(IHTMLWindow4 *iface)
{
    HTMLWindow *This = impl_from_IHTMLWindow4(iface);

    return IHTMLWindow2_Release(&This->IHTMLWindow2_iface);
}

static HRESULT WINAPI HTMLWindow4_GetTypeInfoCount(IHTMLWindow4 *iface, UINT *pctinfo)
{
    HTMLWindow *This = impl_from_IHTMLWindow4(iface);

    return IDispatchEx_GetTypeInfoCount(&This->IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI HTMLWindow4_GetTypeInfo(IHTMLWindow4 *iface, UINT iTInfo,
                                              LCID lcid, ITypeInfo **ppTInfo)
{
    HTMLWindow *This = impl_from_IHTMLWindow4(iface);

    return IDispatchEx_GetTypeInfo(&This->IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI HTMLWindow4_GetIDsOfNames(IHTMLWindow4 *iface, REFIID riid,
                                                LPOLESTR *rgszNames, UINT cNames,
                                                LCID lcid, DISPID *rgDispId)
{
    HTMLWindow *This = impl_from_IHTMLWindow4(iface);

    return IDispatchEx_GetIDsOfNames(&This->IDispatchEx_iface, riid, rgszNames, cNames, lcid,
            rgDispId);
}

static HRESULT WINAPI HTMLWindow4_Invoke(IHTMLWindow4 *iface, DISPID dispIdMember,
                            REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
                            VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLWindow *This = impl_from_IHTMLWindow4(iface);

    return IDispatchEx_Invoke(&This->IDispatchEx_iface, dispIdMember, riid, lcid, wFlags,
            pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI HTMLWindow4_createPopup(IHTMLWindow4 *iface, VARIANT *varArgIn,
                            IDispatch **ppPopup)
{
    HTMLWindow *This = impl_from_IHTMLWindow4(iface);
    FIXME("(%p)->(%p %p)\n", This, varArgIn, ppPopup);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLWindow4_get_frameElement(IHTMLWindow4 *iface, IHTMLFrameBase **p)
{
    HTMLWindow *This = impl_from_IHTMLWindow4(iface);
    TRACE("(%p)->(%p)\n", This, p);

    if(This->frame_element) {
        *p = &This->frame_element->IHTMLFrameBase_iface;
        IHTMLFrameBase_AddRef(*p);
    }else
        *p = NULL;

    return S_OK;
}

static const IHTMLWindow4Vtbl HTMLWindow4Vtbl = {
    HTMLWindow4_QueryInterface,
    HTMLWindow4_AddRef,
    HTMLWindow4_Release,
    HTMLWindow4_GetTypeInfoCount,
    HTMLWindow4_GetTypeInfo,
    HTMLWindow4_GetIDsOfNames,
    HTMLWindow4_Invoke,
    HTMLWindow4_createPopup,
    HTMLWindow4_get_frameElement
};

static inline HTMLWindow *impl_from_IHTMLPrivateWindow(IHTMLPrivateWindow *iface)
{
    return CONTAINING_RECORD(iface, HTMLWindow, IHTMLPrivateWindow_iface);
}

static HRESULT WINAPI HTMLPrivateWindow_QueryInterface(IHTMLPrivateWindow *iface, REFIID riid, void **ppv)
{
    HTMLWindow *This = impl_from_IHTMLPrivateWindow(iface);

    return IHTMLWindow2_QueryInterface(&This->IHTMLWindow2_iface, riid, ppv);
}

static ULONG WINAPI HTMLPrivateWindow_AddRef(IHTMLPrivateWindow *iface)
{
    HTMLWindow *This = impl_from_IHTMLPrivateWindow(iface);

    return IHTMLWindow2_AddRef(&This->IHTMLWindow2_iface);
}

static ULONG WINAPI HTMLPrivateWindow_Release(IHTMLPrivateWindow *iface)
{
    HTMLWindow *This = impl_from_IHTMLPrivateWindow(iface);

    return IHTMLWindow2_Release(&This->IHTMLWindow2_iface);
}

typedef struct {
    task_t header;
    HTMLWindow *window;
    IUri *uri;
} navigate_javascript_task_t;

static void navigate_javascript_proc(task_t *_task)
{
    navigate_javascript_task_t *task = (navigate_javascript_task_t*)_task;
    HTMLWindow *window = task->window;
    VARIANT v;
    BSTR code;
    HRESULT hres;

    static const WCHAR jscriptW[] = {'j','s','c','r','i','p','t',0};

    task->window->readystate = READYSTATE_COMPLETE;

    hres = IUri_GetPath(task->uri, &code);
    if(FAILED(hres))
        return;

    set_download_state(window->doc_obj, 1);

    V_VT(&v) = VT_EMPTY;
    hres = exec_script(window, code, jscriptW, &v);
    SysFreeString(code);
    if(SUCCEEDED(hres) && V_VT(&v) != VT_EMPTY) {
        FIXME("javascirpt URL returned %s\n", debugstr_variant(&v));
        VariantClear(&v);
    }

    if(window->doc_obj->view_sink)
        IAdviseSink_OnViewChange(window->doc_obj->view_sink, DVASPECT_CONTENT, -1);

    set_download_state(window->doc_obj, 0);
}

static void navigate_javascript_task_destr(task_t *_task)
{
    navigate_javascript_task_t *task = (navigate_javascript_task_t*)_task;

    IUri_Release(task->uri);
    heap_free(task);
}

typedef struct {
    task_t header;
    HTMLWindow *window;
    nsChannelBSC *bscallback;
    IMoniker *mon;
} navigate_task_t;

static void navigate_proc(task_t *_task)
{
    navigate_task_t *task = (navigate_task_t*)_task;
    HRESULT hres;

    hres = set_moniker(&task->window->doc_obj->basedoc, task->mon, NULL, task->bscallback, TRUE);
    if(SUCCEEDED(hres))
        hres = start_binding(task->window, NULL, (BSCallback*)task->bscallback, NULL);

}

static void navigate_task_destr(task_t *_task)
{
    navigate_task_t *task = (navigate_task_t*)_task;

    IUnknown_Release((IUnknown*)task->bscallback);
    IMoniker_Release(task->mon);
    heap_free(task);
}

static HRESULT WINAPI HTMLPrivateWindow_SuperNavigate(IHTMLPrivateWindow *iface, BSTR url, BSTR arg2, BSTR arg3,
        BSTR arg4, VARIANT *post_data_var, VARIANT *headers_var, ULONG flags)
{
    HTMLWindow *This = impl_from_IHTMLPrivateWindow(iface);
    DWORD post_data_size = 0;
    BYTE *post_data = NULL;
    WCHAR *headers = NULL;
    nsChannelBSC *bsc;
    IMoniker *mon;
    DWORD scheme;
    BSTR new_url;
    IUri *uri;
    HRESULT hres;

    TRACE("(%p)->(%s %s %s %s %s %s %x)\n", This, debugstr_w(url), debugstr_w(arg2), debugstr_w(arg3), debugstr_w(arg4),
          debugstr_variant(post_data_var), debugstr_variant(headers_var), flags);

    new_url = url;
    if(This->doc_obj->hostui) {
        OLECHAR *translated_url = NULL;

        hres = IDocHostUIHandler_TranslateUrl(This->doc_obj->hostui, 0, url, &translated_url);
        if(hres == S_OK && translated_url) {
            new_url = SysAllocString(translated_url);
            CoTaskMemFree(translated_url);
        }
    }

    if(This->doc_obj->client) {
        IOleCommandTarget *cmdtrg;

        hres = IOleClientSite_QueryInterface(This->doc_obj->client, &IID_IOleCommandTarget, (void**)&cmdtrg);
        if(SUCCEEDED(hres)) {
            VARIANT in, out;

            V_VT(&in) = VT_BSTR;
            V_BSTR(&in) = new_url;
            V_VT(&out) = VT_BOOL;
            V_BOOL(&out) = VARIANT_TRUE;
            hres = IOleCommandTarget_Exec(cmdtrg, &CGID_ShellDocView, 67, 0, &in, &out);
            IOleCommandTarget_Release(cmdtrg);
            if(SUCCEEDED(hres))
                VariantClear(&out);
        }
    }

    hres = CreateUri(new_url, 0, 0, &uri);
    if(new_url != url)
        SysFreeString(new_url);
    if(FAILED(hres))
        return hres;

    hres = CreateURLMonikerEx2(NULL, uri, &mon, URL_MK_UNIFORM);
    if(FAILED(hres))
        return hres;

    /* FIXME: Why not set_ready_state? */
    This->readystate = READYSTATE_UNINITIALIZED;

    if(post_data_var) {
        if(V_VT(post_data_var) == (VT_ARRAY|VT_UI1)) {
            SafeArrayAccessData(V_ARRAY(post_data_var), (void**)&post_data);
            post_data_size = V_ARRAY(post_data_var)->rgsabound[0].cElements;
        }
    }

    if(headers_var && V_VT(headers_var) != VT_EMPTY && V_VT(headers_var) != VT_ERROR) {
        if(V_VT(headers_var) != VT_BSTR)
            return E_INVALIDARG;

        headers = V_BSTR(headers_var);
    }

    hres = create_channelbsc(mon, headers, post_data, post_data_size, &bsc);
    if(post_data)
        SafeArrayUnaccessData(V_ARRAY(post_data_var));
    if(FAILED(hres)) {
        IMoniker_Release(mon);
        return hres;
    }

    prepare_for_binding(&This->doc_obj->basedoc, mon, NULL, TRUE);

    hres = IUri_GetScheme(uri, &scheme);

    if(scheme != URL_SCHEME_JAVASCRIPT) {
        navigate_task_t *task;

        IUri_Release(uri);

        task = heap_alloc(sizeof(*task));
        if(!task) {
            IUnknown_Release((IUnknown*)bsc);
            IMoniker_Release(mon);
            return E_OUTOFMEMORY;
        }

        task->window = This;
        task->bscallback = bsc;
        task->mon = mon;
        push_task(&task->header, navigate_proc, navigate_task_destr, This->task_magic);

        /* Silently and repeated when real loading starts? */
        This->readystate = READYSTATE_LOADING;
    }else {
        navigate_javascript_task_t *task;

        IUnknown_Release((IUnknown*)bsc);
        IMoniker_Release(mon);

        task = heap_alloc(sizeof(*task));
        if(!task) {
            IUri_Release(uri);
            return E_OUTOFMEMORY;
        }

        task->window = This;
        task->uri = uri;
        push_task(&task->header, navigate_javascript_proc, navigate_javascript_task_destr, This->task_magic);

        /* Why silently? */
        This->readystate = READYSTATE_COMPLETE;
    }

    return S_OK;
}

static HRESULT WINAPI HTMLPrivateWindow_GetPendingUrl(IHTMLPrivateWindow *iface, BSTR *url)
{
    HTMLWindow *This = impl_from_IHTMLPrivateWindow(iface);
    FIXME("(%p)->(%p)\n", This, url);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLPrivateWindow_SetPICSTarget(IHTMLPrivateWindow *iface, IOleCommandTarget *cmdtrg)
{
    HTMLWindow *This = impl_from_IHTMLPrivateWindow(iface);
    FIXME("(%p)->(%p)\n", This, cmdtrg);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLPrivateWindow_PICSComplete(IHTMLPrivateWindow *iface, int arg)
{
    HTMLWindow *This = impl_from_IHTMLPrivateWindow(iface);
    FIXME("(%p)->(%x)\n", This, arg);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLPrivateWindow_FindWindowByName(IHTMLPrivateWindow *iface, LPCWSTR name, IHTMLWindow2 **ret)
{
    HTMLWindow *This = impl_from_IHTMLPrivateWindow(iface);
    FIXME("(%p)->(%s %p)\n", This, debugstr_w(name), ret);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLPrivateWindow_GetAddressBar(IHTMLPrivateWindow *iface, BSTR *url)
{
    HTMLWindow *This = impl_from_IHTMLPrivateWindow(iface);
    TRACE("(%p)->(%p)\n", This, url);

    if(!url)
        return E_INVALIDARG;

    *url = SysAllocString(This->url);
    return S_OK;
}

static const IHTMLPrivateWindowVtbl HTMLPrivateWindowVtbl = {
    HTMLPrivateWindow_QueryInterface,
    HTMLPrivateWindow_AddRef,
    HTMLPrivateWindow_Release,
    HTMLPrivateWindow_SuperNavigate,
    HTMLPrivateWindow_GetPendingUrl,
    HTMLPrivateWindow_SetPICSTarget,
    HTMLPrivateWindow_PICSComplete,
    HTMLPrivateWindow_FindWindowByName,
    HTMLPrivateWindow_GetAddressBar
};

static inline HTMLWindow *impl_from_IDispatchEx(IDispatchEx *iface)
{
    return CONTAINING_RECORD(iface, HTMLWindow, IDispatchEx_iface);
}

static HRESULT WINAPI WindowDispEx_QueryInterface(IDispatchEx *iface, REFIID riid, void **ppv)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);

    return IHTMLWindow2_QueryInterface(&This->IHTMLWindow2_iface, riid, ppv);
}

static ULONG WINAPI WindowDispEx_AddRef(IDispatchEx *iface)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);

    return IHTMLWindow2_AddRef(&This->IHTMLWindow2_iface);
}

static ULONG WINAPI WindowDispEx_Release(IDispatchEx *iface)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);

    return IHTMLWindow2_Release(&This->IHTMLWindow2_iface);
}

static HRESULT WINAPI WindowDispEx_GetTypeInfoCount(IDispatchEx *iface, UINT *pctinfo)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);

    TRACE("(%p)->(%p)\n", This, pctinfo);

    return IDispatchEx_GetTypeInfoCount(&This->dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI WindowDispEx_GetTypeInfo(IDispatchEx *iface, UINT iTInfo,
                                               LCID lcid, ITypeInfo **ppTInfo)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);

    TRACE("(%p)->(%u %u %p)\n", This, iTInfo, lcid, ppTInfo);

    return IDispatchEx_GetTypeInfo(&This->dispex.IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI WindowDispEx_GetIDsOfNames(IDispatchEx *iface, REFIID riid,
                                                 LPOLESTR *rgszNames, UINT cNames,
                                                 LCID lcid, DISPID *rgDispId)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);
    UINT i;
    HRESULT hres;

    WARN("(%p)->(%s %p %u %u %p)\n", This, debugstr_guid(riid), rgszNames, cNames,
          lcid, rgDispId);

    for(i=0; i < cNames; i++) {
        /* We shouldn't use script's IDispatchEx here, so we shouldn't use GetDispID */
        hres = IDispatchEx_GetDispID(&This->IDispatchEx_iface, rgszNames[i], 0, rgDispId+i);
        if(FAILED(hres))
            return hres;
    }

    return S_OK;
}

static HRESULT WINAPI WindowDispEx_Invoke(IDispatchEx *iface, DISPID dispIdMember,
                            REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
                            VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);

    TRACE("(%p)->(%d %s %d %d %p %p %p %p)\n", This, dispIdMember, debugstr_guid(riid),
          lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    /* FIXME: Use script dispatch */

    return IDispatchEx_Invoke(&This->dispex.IDispatchEx_iface, dispIdMember, riid, lcid, wFlags,
            pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static global_prop_t *alloc_global_prop(HTMLWindow *This, global_prop_type_t type, BSTR name)
{
    if(This->global_prop_cnt == This->global_prop_size) {
        global_prop_t *new_props;
        DWORD new_size;

        if(This->global_props) {
            new_size = This->global_prop_size*2;
            new_props = heap_realloc(This->global_props, new_size*sizeof(global_prop_t));
        }else {
            new_size = 16;
            new_props = heap_alloc(new_size*sizeof(global_prop_t));
        }
        if(!new_props)
            return NULL;
        This->global_props = new_props;
        This->global_prop_size = new_size;
    }

    This->global_props[This->global_prop_cnt].name = heap_strdupW(name);
    if(!This->global_props[This->global_prop_cnt].name)
        return NULL;

    This->global_props[This->global_prop_cnt].type = type;
    return This->global_props + This->global_prop_cnt++;
}

static inline DWORD prop_to_dispid(HTMLWindow *This, global_prop_t *prop)
{
    return MSHTML_DISPID_CUSTOM_MIN + (prop-This->global_props);
}

HRESULT search_window_props(HTMLWindow *This, BSTR bstrName, DWORD grfdex, DISPID *pid)
{
    DWORD i;
    ScriptHost *script_host;
    DISPID id;

    for(i=0; i < This->global_prop_cnt; i++) {
        /* FIXME: case sensitivity */
        if(!strcmpW(This->global_props[i].name, bstrName)) {
            *pid = MSHTML_DISPID_CUSTOM_MIN+i;
            return S_OK;
        }
    }

    if(find_global_prop(This, bstrName, grfdex, &script_host, &id)) {
        global_prop_t *prop;

        prop = alloc_global_prop(This, GLOBAL_SCRIPTVAR, bstrName);
        if(!prop)
            return E_OUTOFMEMORY;

        prop->script_host = script_host;
        prop->id = id;

        *pid = prop_to_dispid(This, prop);
        return S_OK;
    }

    return DISP_E_UNKNOWNNAME;
}

static HRESULT WINAPI WindowDispEx_GetDispID(IDispatchEx *iface, BSTR bstrName, DWORD grfdex, DISPID *pid)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);
    HRESULT hres;

    TRACE("(%p)->(%s %x %p)\n", This, debugstr_w(bstrName), grfdex, pid);

    hres = search_window_props(This, bstrName, grfdex, pid);
    if(hres != DISP_E_UNKNOWNNAME)
        return hres;

    hres = IDispatchEx_GetDispID(&This->dispex.IDispatchEx_iface, bstrName, grfdex, pid);
    if(hres != DISP_E_UNKNOWNNAME)
        return hres;

    if(This->doc) {
        global_prop_t *prop;
        IHTMLElement *elem;

        hres = IHTMLDocument3_getElementById(&This->doc->basedoc.IHTMLDocument3_iface,
                                             bstrName, &elem);
        if(SUCCEEDED(hres) && elem) {
            IHTMLElement_Release(elem);

            prop = alloc_global_prop(This, GLOBAL_ELEMENTVAR, bstrName);
            if(!prop)
                return E_OUTOFMEMORY;

            *pid = prop_to_dispid(This, prop);
            return S_OK;
        }
    }

    return DISP_E_UNKNOWNNAME;
}

static HRESULT WINAPI WindowDispEx_InvokeEx(IDispatchEx *iface, DISPID id, LCID lcid, WORD wFlags, DISPPARAMS *pdp,
        VARIANT *pvarRes, EXCEPINFO *pei, IServiceProvider *pspCaller)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);

    TRACE("(%p)->(%x %x %x %p %p %p %p)\n", This, id, lcid, wFlags, pdp, pvarRes, pei, pspCaller);

    if(id == DISPID_IHTMLWINDOW2_LOCATION && (wFlags & DISPATCH_PROPERTYPUT)) {
        HTMLLocation *location;
        HRESULT hres;

        TRACE("forwarding to location.href\n");

        hres = get_location(This, &location);
        if(FAILED(hres))
            return hres;

        hres = IDispatchEx_InvokeEx(&location->dispex.IDispatchEx_iface, DISPID_VALUE, lcid,
                wFlags, pdp, pvarRes, pei, pspCaller);
        IHTMLLocation_Release(&location->IHTMLLocation_iface);
        return hres;
    }

    return IDispatchEx_InvokeEx(&This->dispex.IDispatchEx_iface, id, lcid, wFlags, pdp, pvarRes,
            pei, pspCaller);
}

static HRESULT WINAPI WindowDispEx_DeleteMemberByName(IDispatchEx *iface, BSTR bstrName, DWORD grfdex)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);

    TRACE("(%p)->(%s %x)\n", This, debugstr_w(bstrName), grfdex);

    return IDispatchEx_DeleteMemberByName(&This->dispex.IDispatchEx_iface, bstrName, grfdex);
}

static HRESULT WINAPI WindowDispEx_DeleteMemberByDispID(IDispatchEx *iface, DISPID id)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);

    TRACE("(%p)->(%x)\n", This, id);

    return IDispatchEx_DeleteMemberByDispID(&This->dispex.IDispatchEx_iface, id);
}

static HRESULT WINAPI WindowDispEx_GetMemberProperties(IDispatchEx *iface, DISPID id, DWORD grfdexFetch, DWORD *pgrfdex)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);

    TRACE("(%p)->(%x %x %p)\n", This, id, grfdexFetch, pgrfdex);

    return IDispatchEx_GetMemberProperties(&This->dispex.IDispatchEx_iface, id, grfdexFetch,
            pgrfdex);
}

static HRESULT WINAPI WindowDispEx_GetMemberName(IDispatchEx *iface, DISPID id, BSTR *pbstrName)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);

    TRACE("(%p)->(%x %p)\n", This, id, pbstrName);

    return IDispatchEx_GetMemberName(&This->dispex.IDispatchEx_iface, id, pbstrName);
}

static HRESULT WINAPI WindowDispEx_GetNextDispID(IDispatchEx *iface, DWORD grfdex, DISPID id, DISPID *pid)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);

    TRACE("(%p)->(%x %x %p)\n", This, grfdex, id, pid);

    return IDispatchEx_GetNextDispID(&This->dispex.IDispatchEx_iface, grfdex, id, pid);
}

static HRESULT WINAPI WindowDispEx_GetNameSpaceParent(IDispatchEx *iface, IUnknown **ppunk)
{
    HTMLWindow *This = impl_from_IDispatchEx(iface);

    TRACE("(%p)->(%p)\n", This, ppunk);

    *ppunk = NULL;
    return S_OK;
}

static const IDispatchExVtbl WindowDispExVtbl = {
    WindowDispEx_QueryInterface,
    WindowDispEx_AddRef,
    WindowDispEx_Release,
    WindowDispEx_GetTypeInfoCount,
    WindowDispEx_GetTypeInfo,
    WindowDispEx_GetIDsOfNames,
    WindowDispEx_Invoke,
    WindowDispEx_GetDispID,
    WindowDispEx_InvokeEx,
    WindowDispEx_DeleteMemberByName,
    WindowDispEx_DeleteMemberByDispID,
    WindowDispEx_GetMemberProperties,
    WindowDispEx_GetMemberName,
    WindowDispEx_GetNextDispID,
    WindowDispEx_GetNameSpaceParent
};

static inline HTMLWindow *impl_from_IServiceProvider(IServiceProvider *iface)
{
    return CONTAINING_RECORD(iface, HTMLWindow, IServiceProvider_iface);
}

static HRESULT WINAPI HTMLWindowSP_QueryInterface(IServiceProvider *iface, REFIID riid, void **ppv)
{
    HTMLWindow *This = impl_from_IServiceProvider(iface);
    return IHTMLWindow2_QueryInterface(&This->IHTMLWindow2_iface, riid, ppv);
}

static ULONG WINAPI HTMLWindowSP_AddRef(IServiceProvider *iface)
{
    HTMLWindow *This = impl_from_IServiceProvider(iface);
    return IHTMLWindow2_AddRef(&This->IHTMLWindow2_iface);
}

static ULONG WINAPI HTMLWindowSP_Release(IServiceProvider *iface)
{
    HTMLWindow *This = impl_from_IServiceProvider(iface);
    return IHTMLWindow2_Release(&This->IHTMLWindow2_iface);
}

static HRESULT WINAPI HTMLWindowSP_QueryService(IServiceProvider *iface, REFGUID guidService, REFIID riid, void **ppv)
{
    HTMLWindow *This = impl_from_IServiceProvider(iface);

    if(IsEqualGUID(guidService, &IID_IHTMLWindow2)) {
        TRACE("IID_IHTMLWindow2\n");
        return IHTMLWindow2_QueryInterface(&This->IHTMLWindow2_iface, riid, ppv);
    }

    TRACE("(%p)->(%s %s %p)\n", This, debugstr_guid(guidService), debugstr_guid(riid), ppv);

    if(!This->doc_obj)
        return E_NOINTERFACE;

    return IServiceProvider_QueryService(&This->doc_obj->basedoc.IServiceProvider_iface,
            guidService, riid, ppv);
}

static const IServiceProviderVtbl ServiceProviderVtbl = {
    HTMLWindowSP_QueryInterface,
    HTMLWindowSP_AddRef,
    HTMLWindowSP_Release,
    HTMLWindowSP_QueryService
};

static inline HTMLWindow *impl_from_DispatchEx(DispatchEx *iface)
{
    return CONTAINING_RECORD(iface, HTMLWindow, dispex);
}

static HRESULT HTMLWindow_invoke(DispatchEx *dispex, DISPID id, LCID lcid, WORD flags, DISPPARAMS *params,
        VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    HTMLWindow *This = impl_from_DispatchEx(dispex);
    global_prop_t *prop;
    DWORD idx;
    HRESULT hres;

    idx = id - MSHTML_DISPID_CUSTOM_MIN;
    if(idx >= This->global_prop_cnt)
        return DISP_E_MEMBERNOTFOUND;

    prop = This->global_props+idx;

    switch(prop->type) {
    case GLOBAL_SCRIPTVAR: {
        IDispatchEx *iface;
        IDispatch *disp;

        disp = get_script_disp(prop->script_host);
        if(!disp)
            return E_UNEXPECTED;

        hres = IDispatch_QueryInterface(disp, &IID_IDispatchEx, (void**)&iface);
        if(SUCCEEDED(hres)) {
            TRACE("%s >>>\n", debugstr_w(prop->name));
            hres = IDispatchEx_InvokeEx(iface, prop->id, lcid, flags, params, res, ei, caller);
            if(hres == S_OK)
                TRACE("%s <<<\n", debugstr_w(prop->name));
            else
                WARN("%s <<< %08x\n", debugstr_w(prop->name), hres);
            IDispatchEx_Release(iface);
        }else {
            FIXME("No IDispatchEx\n");
        }
        IDispatch_Release(disp);
        break;
    }
    case GLOBAL_ELEMENTVAR: {
        IHTMLElement *elem;

        hres = IHTMLDocument3_getElementById(&This->doc->basedoc.IHTMLDocument3_iface,
                                             prop->name, &elem);
        if(FAILED(hres))
            return hres;

        if(!elem)
            return DISP_E_MEMBERNOTFOUND;

        V_VT(res) = VT_DISPATCH;
        V_DISPATCH(res) = (IDispatch*)elem;
        break;
    }
    default:
        ERR("invalid type %d\n", prop->type);
        hres = DISP_E_MEMBERNOTFOUND;
    }

    return hres;
}


static const dispex_static_data_vtbl_t HTMLWindow_dispex_vtbl = {
    NULL,
    NULL,
    HTMLWindow_invoke,
    NULL
};

static const tid_t HTMLWindow_iface_tids[] = {
    IHTMLWindow2_tid,
    IHTMLWindow3_tid,
    IHTMLWindow4_tid,
    0
};

static dispex_static_data_t HTMLWindow_dispex = {
    &HTMLWindow_dispex_vtbl,
    DispHTMLWindow2_tid,
    NULL,
    HTMLWindow_iface_tids
};

HRESULT HTMLWindow_Create(HTMLDocumentObj *doc_obj, nsIDOMWindow *nswindow, HTMLWindow *parent, HTMLWindow **ret)
{
    HTMLWindow *window;
    HRESULT hres;

    window = heap_alloc_zero(sizeof(HTMLWindow));
    if(!window)
        return E_OUTOFMEMORY;

    window->window_ref = heap_alloc(sizeof(windowref_t));
    if(!window->window_ref) {
        heap_free(window);
        return E_OUTOFMEMORY;
    }

    window->IHTMLWindow2_iface.lpVtbl = &HTMLWindow2Vtbl;
    window->IHTMLWindow3_iface.lpVtbl = &HTMLWindow3Vtbl;
    window->IHTMLWindow4_iface.lpVtbl = &HTMLWindow4Vtbl;
    window->IHTMLPrivateWindow_iface.lpVtbl = &HTMLPrivateWindowVtbl;
    window->IDispatchEx_iface.lpVtbl = &WindowDispExVtbl;
    window->IServiceProvider_iface.lpVtbl = &ServiceProviderVtbl;
    window->ref = 1;
    window->doc_obj = doc_obj;

    window->window_ref->window = window;
    window->window_ref->ref = 1;

    init_dispex(&window->dispex, (IUnknown*)&window->IHTMLWindow2_iface, &HTMLWindow_dispex);

    if(nswindow) {
        nsIDOMWindow_AddRef(nswindow);
        window->nswindow = nswindow;
    }

    window->scriptmode = parent ? parent->scriptmode : SCRIPTMODE_GECKO;
    window->readystate = READYSTATE_UNINITIALIZED;
    list_init(&window->script_hosts);

    hres = CoInternetCreateSecurityManager(NULL, &window->secmgr, 0);
    if(FAILED(hres)) {
        IHTMLWindow2_Release(&window->IHTMLWindow2_iface);
        return hres;
    }

    window->task_magic = get_task_target_magic();
    update_window_doc(window);

    list_init(&window->children);
    list_add_head(&window_list, &window->entry);

    if(parent) {
        IHTMLWindow2_AddRef(&window->IHTMLWindow2_iface);

        window->parent = parent;
        list_add_tail(&parent->children, &window->sibling_entry);
    }

    *ret = window;
    return S_OK;
}

void update_window_doc(HTMLWindow *window)
{
    nsIDOMHTMLDocument *nshtmldoc;
    nsIDOMDocument *nsdoc;
    nsresult nsres;

    nsres = nsIDOMWindow_GetDocument(window->nswindow, &nsdoc);
    if(NS_FAILED(nsres) || !nsdoc) {
        ERR("GetDocument failed: %08x\n", nsres);
        return;
    }

    nsres = nsIDOMDocument_QueryInterface(nsdoc, &IID_nsIDOMHTMLDocument, (void**)&nshtmldoc);
    nsIDOMDocument_Release(nsdoc);
    if(NS_FAILED(nsres)) {
        ERR("Could not get nsIDOMHTMLDocument iface: %08x\n", nsres);
        return;
    }

    if(!window->doc || window->doc->nsdoc != nshtmldoc) {
        HTMLDocumentNode *doc;
        HRESULT hres;

        hres = create_doc_from_nsdoc(nshtmldoc, window->doc_obj, window, &doc);
        if(SUCCEEDED(hres)) {
            window_set_docnode(window, doc);
            htmldoc_release(&doc->basedoc);
        }else {
            ERR("create_doc_from_nsdoc failed: %08x\n", hres);
        }
    }

    nsIDOMHTMLDocument_Release(nshtmldoc);
}

HTMLWindow *nswindow_to_window(const nsIDOMWindow *nswindow)
{
    HTMLWindow *iter;

    LIST_FOR_EACH_ENTRY(iter, &window_list, HTMLWindow, entry) {
        if(iter->nswindow == nswindow)
            return iter;
    }

    return NULL;
}
