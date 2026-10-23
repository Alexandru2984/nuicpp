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

  if (globalThis.NuiGraphReview) {
    attachEditor();
  } else {
    const review = document.createElement("script");
    review.src = `/diagram-review.js${assetQuery}`;
    review.defer = true;
    review.onload = attachEditor;
    // The editor remains usable if the optional review engine is unavailable;
    // its command reports that state instead of blocking the whole canvas.
    review.onerror = attachEditor;
    document.head.appendChild(review);
  }
})();
