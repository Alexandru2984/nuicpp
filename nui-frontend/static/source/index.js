const nuigraphAssetQuery = document.currentScript ? new URL(document.currentScript.src).search : "";

window.addEventListener("load", () => {
  const attachEditor = () => {
    const script = document.createElement("script");
    script.src = `/editor.js${nuigraphAssetQuery}`;
    script.defer = true;
    document.body.appendChild(script);
  };
  if (globalThis.NuiGraphReview) {
    attachEditor();
    return;
  }
  const review = document.createElement("script");
  review.src = `/diagram-review.js${nuigraphAssetQuery}`;
  review.defer = true;
  review.onload = attachEditor;
  review.onerror = attachEditor;
  document.body.appendChild(review);
});
