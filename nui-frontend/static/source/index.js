const nuigraphAssetQuery = document.currentScript ? new URL(document.currentScript.src).search : "";

window.addEventListener("load", () => {
  const attachEditor = () => {
    const script = document.createElement("script");
    script.src = `/editor.js${nuigraphAssetQuery}`;
    script.defer = true;
    document.body.appendChild(script);
  };

  const attachRulers = () => {
    if (globalThis.NuiGraphRulers) {
      attachEditor();
      return;
    }
    const rulers = document.createElement("script");
    rulers.src = `/canvas-rulers.js${nuigraphAssetQuery}`;
    rulers.defer = true;
    rulers.onload = attachEditor;
    rulers.onerror = attachEditor;
    document.body.appendChild(rulers);
  };

  const attachSelection = () => {
    if (globalThis.NuiGraphSelection) {
      attachRulers();
      return;
    }
    const selection = document.createElement("script");
    selection.src = `/diagram-selection.js${nuigraphAssetQuery}`;
    selection.defer = true;
    selection.onload = attachRulers;
    selection.onerror = attachRulers;
    document.body.appendChild(selection);
  };

  const attachArrange = () => {
    if (globalThis.NuiGraphArrange) {
      attachSelection();
      return;
    }
    const arrange = document.createElement("script");
    arrange.src = `/diagram-arrange.js${nuigraphAssetQuery}`;
    arrange.defer = true;
    arrange.onload = attachSelection;
    arrange.onerror = attachSelection;
    document.body.appendChild(arrange);
  };

  if (globalThis.NuiGraphReview) {
    attachArrange();
    return;
  }
  const review = document.createElement("script");
  review.src = `/diagram-review.js${nuigraphAssetQuery}`;
  review.defer = true;
  review.onload = attachArrange;
  review.onerror = attachArrange;
  document.body.appendChild(review);
});
