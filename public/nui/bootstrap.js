const nuigraphAssetQuery = document.currentScript ? new URL(document.currentScript.src).search : "";

window.addEventListener("load", () => {
  fetch("/api/session", { credentials: "same-origin" }).catch(() => {});

  const attachShellEditor = () => {
    if (!document.getElementById("app")) {
      window.setTimeout(attachShellEditor, 30);
      return;
    }
    const attachEditor = () => {
      const script = document.createElement("script");
      script.src = `/editor.js${nuigraphAssetQuery}`;
      document.body.appendChild(script);
    };
    if (globalThis.NuiGraphReview) {
      attachEditor();
      return;
    }
    const review = document.createElement("script");
    review.src = `/diagram-review.js${nuigraphAssetQuery}`;
    review.onload = attachEditor;
    review.onerror = attachEditor;
    document.body.appendChild(review);
  };

  attachShellEditor();
});
