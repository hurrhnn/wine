/*
 *	OLE2 library
 *
 *	Copyright 1995	Martin von Loewis
 */

#include "windows.h"
#include "winerror.h"
#include "ole2.h"
#include "process.h"
#include "debug.h"
#include "objbase.h"
#include "objidl.h"
#include "wine/obj_base.h"
#include "wine/obj_clientserver.h"
#include "wine/obj_storage.h"
#include "wine/obj_moniker.h"

/******************************************************************************
 * These are static/global variables that the OLE module uses to maintain
 * it's state.
 */

/*
 * This is the lock count on the OLE library. It is controlled by the
 * OLEInitialize/OLEUninitialize methods.
 */
static ULONG s_OLEModuleLockCount = 0;

/******************************************************************************
 *		OleBuildVersion	[OLE2.1]
 */
DWORD WINAPI OleBuildVersion(void)
{
    TRACE(ole,"(void)\n");
    return (rmm<<16)+rup;
}

/***********************************************************************
 *           OleInitialize       (OLE2.2) (OLE32.108)
 */
HRESULT WINAPI OleInitialize(LPVOID reserved)
{
  HRESULT hr;

  TRACE(ole, "(%p)\n", reserved);

  /*
   * The first duty of the OleInitialize is to initialize the COM libraries.
   */
  hr = CoInitializeEx32(NULL, COINIT_APARTMENTTHREADED);

  /*
   * If the CoInitializeEx call failed, the OLE libraries can't be 
   * initialized.
   */
  if (FAILED(hr))
    return hr;    

  /*
   * Then, it has to initialize the OLE specific modules.
   * This includes:
   *     Clipboard
   *     Drag and Drop
   *     Object linking and Embedding
   *     In-place activation
   */
  if (s_OLEModuleLockCount==0)
{
    /* 
     * Initialize the libraries.
     */
    TRACE(ole, "() - Initializing the OLE libraries\n");
}

  /*
   * Then, we increase the lock count on the OLE module.
   */
  s_OLEModuleLockCount++;  

  return hr;
}

/******************************************************************************
 *		CoGetCurrentProcess	[COMPOBJ.34] [OLE2.2][OLE32.108]
 *
 * NOTES
 *   Is DWORD really the correct return type for this function?
 */
DWORD WINAPI CoGetCurrentProcess(void) {
	return (DWORD)PROCESS_Current();
}

/******************************************************************************
 *		OleUninitialize	[OLE2.3] [OLE32.131]
 */
void WINAPI OleUninitialize(void)
{
  TRACE(ole, "()\n");

  /*
   * Decrease the lock count on the OLE module.
   */
  s_OLEModuleLockCount--;

  /*
   * If we hit the bottom of the lock stack, free the libraries.
   */
  if (s_OLEModuleLockCount==0)
  {
    /*
     * Actually free the libraries.
     */
    TRACE(ole, "() - Freeing the last reference count\n");
  }
  
  /*
   * Then, uninitialize the COM libraries.
   */
  CoUninitialize32();
}

/***********************************************************************
 *           OleFlushClipboard   [OLE2.76]
 */
HRESULT WINAPI OleFlushClipboard(void)
{
    return S_OK;
}

/***********************************************************************
 *           OleSetClipboard     [OLE32.127]
 */
HRESULT WINAPI OleSetClipboard(LPVOID pDataObj)
{
    FIXME(ole,"(%p), stub!\n", pDataObj);
    return S_OK;
}

/******************************************************************************
 *		CoRegisterMessageFilter32	[OLE32.38]
 */
HRESULT WINAPI CoRegisterMessageFilter32(
    LPMESSAGEFILTER lpMessageFilter,	/* Pointer to interface */
    LPMESSAGEFILTER *lplpMessageFilter	/* Indirect pointer to prior instance if non-NULL */
) {
    FIXME(ole,"stub\n");
    if (lplpMessageFilter) {
	*lplpMessageFilter = NULL;
    }
    return S_OK;
}

/******************************************************************************
 *		OleInitializeWOW	[OLE32.109]
 */
HRESULT WINAPI OleInitializeWOW(DWORD x) {
        FIXME(ole,"(0x%08lx),stub!\n",x);
        return 0;
}

/***********************************************************************
 *           RegisterDragDrop16 (OLE2.35)
 */
HRESULT WINAPI RegisterDragDrop16(
	HWND16 hwnd,
	LPDROPTARGET pDropTarget
) {
	FIXME(ole,"(0x%04x,%p),stub!\n",hwnd,pDropTarget);
	return S_OK;
}

/***********************************************************************
 *           RegisterDragDrop32 (OLE32.139)
 */
HRESULT WINAPI RegisterDragDrop32(
	HWND32 hwnd,
	LPDROPTARGET pDropTarget
) {
	FIXME(ole,"(0x%04x,%p),stub!\n",hwnd,pDropTarget);
	return S_OK;
}

/***********************************************************************
 *           RevokeDragDrop16 (OLE2.36)
 */
HRESULT WINAPI RevokeDragDrop16(
	HWND16 hwnd
) {
	FIXME(ole,"(0x%04x),stub!\n",hwnd);
	return S_OK;
}

/***********************************************************************
 *           RevokeDragDrop32 (OLE32.141)
 */
HRESULT WINAPI RevokeDragDrop32(
	HWND32 hwnd
) {
	FIXME(ole,"(0x%04x),stub!\n",hwnd);
	return S_OK;
}

/***********************************************************************
 *           OleRegGetUserType (OLE32.122)
 */
HRESULT WINAPI OleRegGetUserType32( 
	REFCLSID clsid, 
	DWORD dwFormOfType,
	LPOLESTR32* pszUserType)
{
	FIXME(ole,",stub!\n");
	return S_OK;
}

/***********************************************************************
 * DoDragDrop32 [OLE32.65]
 */
HRESULT WINAPI DoDragDrop32 (
  IDataObject *pDataObject,  /* ptr to the data obj           */
  IDataObject *pDropSource,  /* ptr to the source obj         */
  DWORD       dwOKEffect,    /* effects allowed by the source */
  DWORD       *pdwEffect)    /* ptr to effects of the source  */
{
  FIXME(ole,"(DataObject %p, DropSource %p): stub!\n", pDataObject, pDropSource);
  return DRAGDROP_S_DROP;
}
