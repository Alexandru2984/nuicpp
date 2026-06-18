window.addEventListener("load", () => {
  fetch("/api/session", { credentials: "same-origin" }).catch(() => {});

  const attachEditor = () => {
    if (!document.getElementById("app")) {
      window.setTimeout(attachEditor, 30);
      return;
    }
    const script = document.createElement("script");
    script.src = "/editor.js";
    document.body.appendChild(script);
  };

  attachEditor();
});
