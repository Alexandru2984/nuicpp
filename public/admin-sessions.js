(() => {
  "use strict";

  const list = document.getElementById("admin-session-list");
  const status = document.getElementById("session-status");
  let csrfToken = "";

  function formatDate(value) {
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? value : date.toLocaleString();
  }

  async function request(path, options = {}) {
    const response = await fetch(path, { credentials: "same-origin", ...options });
    if (response.status === 401 || response.status === 403) {
      location.assign("/login");
      throw new Error("Administrator session required");
    }
    const body = await response.json();
    if (!response.ok) throw new Error(body.error || "Request failed");
    return body;
  }

  function addDetail(container, label, value) {
    const row = document.createElement("div");
    const term = document.createElement("strong");
    const detail = document.createElement("span");
    term.textContent = label;
    detail.textContent = value;
    row.append(term, detail);
    container.append(row);
  }

  async function revoke(id, current, button) {
    const message = current
      ? "Revoke this session and sign out now?"
      : "Revoke this administrator session?";
    if (!window.confirm(message)) return;
    button.disabled = true;
    try {
      const result = await request(`/api/admin/sessions/${id}`, {
        method: "DELETE",
        headers: { "X-CSRF-Token": csrfToken }
      });
      if (result.current_revoked) {
        location.assign("/login");
        return;
      }
      await load();
      status.textContent = "Session revoked.";
    } catch (error) {
      status.textContent = error.message;
      button.disabled = false;
    }
  }

  function render(sessions) {
    list.replaceChildren();
    for (const session of sessions) {
      const card = document.createElement("article");
      card.className = "admin-session-card";
      const heading = document.createElement("div");
      heading.className = "admin-session-heading";
      const title = document.createElement("h2");
      title.textContent = session.device_label;
      heading.append(title);
      if (session.current) {
        const badge = document.createElement("span");
        badge.className = "session-current-badge";
        badge.textContent = "This device";
        heading.append(badge);
      }
      const details = document.createElement("div");
      details.className = "admin-session-details";
      addDetail(details, "Created", formatDate(session.created_at));
      addDetail(details, "Last active", formatDate(session.last_seen_at));
      addDetail(details, "Expires", formatDate(session.expires_at));
      const button = document.createElement("button");
      button.type = "button";
      button.className = "danger";
      button.textContent = session.current ? "Revoke and sign out" : "Revoke session";
      button.addEventListener("click", () => revoke(session.id, session.current, button));
      card.append(heading, details, button);
      list.append(card);
    }
    status.textContent = sessions.length
      ? `${sessions.length} active administrator session${sessions.length === 1 ? "" : "s"}.`
      : "No active administrator sessions.";
  }

  async function load() {
    const session = await request("/api/session");
    csrfToken = session.csrf_token;
    const data = await request("/api/admin/sessions");
    render(data.sessions);
  }

  load().catch((error) => { status.textContent = error.message; });
})();
