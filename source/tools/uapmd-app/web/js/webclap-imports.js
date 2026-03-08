function getWebclapBridge() {
  if (typeof globalThis === 'undefined') return null;
  return globalThis.uapmdWebClap || null;
}

function callBridge(method, args, defaultValue) {
  const bridge = getWebclapBridge();
  if (!bridge || typeof bridge[method] !== 'function') {
    if (typeof Module !== 'undefined' && Module.printErr) {
      Module.printErr(`[WebCLAP] Bridge method ${method} is unavailable.`);
    }
    return defaultValue;
  }
  try {
    return bridge[method](...args);
  } catch (err) {
    if (typeof Module !== 'undefined' && Module.printErr) {
      Module.printErr(`[WebCLAP] Bridge method ${method} threw ${err}`);
    }
    return defaultValue;
  }
}

function voidInvoker(method) {
  return function(...args) {
    callBridge(method, args, undefined);
  };
}

function intInvoker(method) {
  return function(...args) {
    return callBridge(method, args, 0) | 0;
  };
}

const webclapLibraryFuncs = {
  _runThread: voidInvoker('runThread'),
  runThread: voidInvoker('runThread'),
  _release: voidInvoker('releaseInstance'),
  release: voidInvoker('releaseInstance'),
  _init32: intInvoker('init32'),
  init32: intInvoker('init32'),
  _init64: intInvoker('init64'),
  init64: intInvoker('init64'),
  _malloc32: intInvoker('malloc32'),
  malloc32: intInvoker('malloc32'),
  _malloc64: intInvoker('malloc64'),
  malloc64: intInvoker('malloc64'),
  _memcpyToOther32: intInvoker('memcpyToOther32'),
  memcpyToOther32: intInvoker('memcpyToOther32'),
  _memcpyToOther64: intInvoker('memcpyToOther64'),
  memcpyToOther64: intInvoker('memcpyToOther64'),
  _memcpyFromOther32: intInvoker('memcpyFromOther32'),
  memcpyFromOther32: intInvoker('memcpyFromOther32'),
  _memcpyFromOther64: intInvoker('memcpyFromOther64'),
  memcpyFromOther64: intInvoker('memcpyFromOther64'),
  _countUntil32: intInvoker('countUntil32'),
  countUntil32: intInvoker('countUntil32'),
  _countUntil64: intInvoker('countUntil64'),
  countUntil64: intInvoker('countUntil64'),
  _call32: intInvoker('call32'),
  call32: intInvoker('call32'),
  _call64: intInvoker('call64'),
  call64: intInvoker('call64'),
  _registerHost32: intInvoker('registerHost32'),
  registerHost32: intInvoker('registerHost32'),
  _registerHost64: intInvoker('registerHost64'),
  registerHost64: intInvoker('registerHost64')
};

mergeInto(LibraryManager.library, webclapLibraryFuncs);
