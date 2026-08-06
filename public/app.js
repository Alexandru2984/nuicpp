(() => {
  if (window.__nuigraphEditorLoading) return;
  window.__nuigraphEditorLoading = true;

  // Registered from a file rather than an inline script so the page needs no
  // 'unsafe-inline' in its Content-Security-Policy.
  if ("serviceWorker" in navigator) {
    window.addEventListener("load", () => {
      navigator.serviceWorker.register("/sw.js").catch(() => {});
    });
  }

  const script = document.createElement("script");
  script.src = "/editor.js";
  script.defer = true;
  document.head.appendChild(script);
})();
