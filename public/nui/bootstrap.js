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

    const attachSelection = () => {
      if (globalThis.NuiGraphSelection) {
        attachEditor();
        return;
      }
      const selection = document.createElement("script");
      selection.src = `/diagram-selection.js${nuigraphAssetQuery}`;
      selection.onload = attachEditor;
      selection.onerror = attachEditor;
      document.body.appendChild(selection);
    };

    const attachArrange = () => {
      if (globalThis.NuiGraphArrange) {
        attachSelection();
        return;
      }
      const arrange = document.createElement("script");
      arrange.src = `/diagram-arrange.js${nuigraphAssetQuery}`;
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
    review.onload = attachArrange;
    review.onerror = attachArrange;
    document.body.appendChild(review);
  };

  attachShellEditor();
});
