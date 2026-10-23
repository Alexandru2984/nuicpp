const nuigraphAssetQuery = document.currentScript ? new URL(document.currentScript.src).search : "";

window.addEventListener("load", () => {
  const script = document.createElement("script");
  script.src = `/editor.js${nuigraphAssetQuery}`;
  script.defer = true;
  document.body.appendChild(script);
});
