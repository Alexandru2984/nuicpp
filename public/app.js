(() => {
  if (window.__nuigraphEditorLoading) return;
  window.__nuigraphEditorLoading = true;
  const assetQuery = document.currentScript ? new URL(document.currentScript.src).search : "";

  // Registered from a file rather than an inline script so the page needs no
  // 'unsafe-inline' in its Content-Security-Policy.
  if ("serviceWorker" in navigator) {
    window.addEventListener("load", () => {
      navigator.serviceWorker.register(`/sw.js${assetQuery}`, { updateViaCache: "none" }).catch(() => {});
    });
  }

  const attachEditor = () => {
    const script = document.createElement("script");
    script.src = `/editor.js${assetQuery}`;
    script.defer = true;
    document.head.appendChild(script);
  };

  const attachArrange = () => {
    if (globalThis.NuiGraphArrange) {
      attachEditor();
      return;
    }
    const arrange = document.createElement("script");
    arrange.src = `/diagram-arrange.js${assetQuery}`;
    arrange.defer = true;
    arrange.onload = attachEditor;
    // A missing optional engine must not prevent basic editing. Its commands
    // remain visible but report that the release assets need to be reloaded.
    arrange.onerror = attachEditor;
    document.head.appendChild(arrange);
  };

  if (globalThis.NuiGraphReview) {
    attachArrange();
  } else {
    const review = document.createElement("script");
    review.src = `/diagram-review.js${assetQuery}`;
    review.defer = true;
    review.onload = attachArrange;
    // The editor remains usable if the optional review engine is unavailable;
    // its command reports that state instead of blocking the whole canvas.
    review.onerror = attachArrange;
    document.head.appendChild(review);
  }
})();
