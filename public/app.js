(() => {
  if (window.__nuigraphEditorLoading) return;
  window.__nuigraphEditorLoading = true;

  const script = document.createElement("script");
  script.src = "/editor.js";
  script.defer = true;
  document.head.appendChild(script);
})();
