window.addEventListener("load", () => {
  const script = document.createElement("script");
  script.src = "/editor.js";
  script.defer = true;
  document.body.appendChild(script);
});
