(function () {
  const API_BASE_KEY = "sheaf_api_base_url";
  const ACTIVE_CHAT_KEY = "sheaf_active_chat_id";
  const DEFAULT_API_BASE = "http://127.0.0.1:2731";

  function getApiBase() {
    return localStorage.getItem(API_BASE_KEY) || DEFAULT_API_BASE;
  }

  function findInput() {
    return document.querySelector("textarea") || null;
  }

  function findSendButton() {
    return (
      document.querySelector('button[type="submit"]') ||
      document.querySelector('button[aria-label*="Send"]') ||
      document.querySelector('button[data-testid*="send"]') ||
      null
    );
  }

  function clickSend() {
    const btn = findSendButton();
    if (btn) {
      btn.click();
      return true;
    }
    return false;
  }

  function forceSendCurrentInput() {
    if (!clickSend()) {
      const input = findInput();
      if (!input) return;
      input.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true, cancelable: true }));
      input.dispatchEvent(new KeyboardEvent("keyup", { key: "Enter", bubbles: true, cancelable: true }));
    }
  }

  function sendCommand(text) {
    const input = findInput();
    if (!input) return;

    input.focus();
    input.value = text;
    input.dispatchEvent(new Event("input", { bubbles: true }));
    forceSendCurrentInput();
  }

  function sendWindowMessage(payload) {
    try {
      window.postMessage(payload, "*");
      return true;
    } catch (_err) {
      return false;
    }
  }

  function parseActiveChatFromText(text) {
    const m = text.match(/(?:Active chat|Switched to(?: new)? chat):\s*`?([a-f0-9-]{8,})`?/i);
    return m ? m[1] : null;
  }

  function renderSidebar(chats) {
    let root = document.getElementById("sheaf-sidebar");
    if (!root) {
      root = document.createElement("aside");
      root.id = "sheaf-sidebar";
      root.innerHTML = [
        '<div class="sheaf-side-header">',
        '  <div class="sheaf-side-title">Chats</div>',
        '  <div class="sheaf-side-actions">',
        '    <button id="sheaf-chat-new" type="button">New</button>',
        '    <button id="sheaf-chat-refresh" type="button">Refresh</button>',
        '    <button id="sheaf-chat-reboot" type="button">Reboot</button>',
        "  </div>",
        "</div>",
        '<div class="sheaf-side-body" id="sheaf-chat-list"></div>'
      ].join("");
      document.body.appendChild(root);

      const newBtn = document.getElementById("sheaf-chat-new");
      const refreshBtn = document.getElementById("sheaf-chat-refresh");
      const rebootBtn = document.getElementById("sheaf-chat-reboot");
      if (newBtn) {
        newBtn.addEventListener("click", function () {
          if (!sendWindowMessage({ type: "sheaf_new_chat" })) {
            sendCommand("/new");
          }
          setTimeout(loadChats, 800);
        });
      }
      if (refreshBtn) {
        refreshBtn.addEventListener("click", loadChats);
      }
      if (rebootBtn) {
        rebootBtn.addEventListener("click", async function () {
          rebootBtn.disabled = true;
          rebootBtn.textContent = "Rebooting...";
          try {
            const resp = await fetch(getApiBase() + "/admin/reboot", { method: "POST" });
            if (!resp.ok) {
              throw new Error("Request failed with status " + resp.status);
            }
          } catch (err) {
            rebootBtn.disabled = false;
            rebootBtn.textContent = "Reboot";
            return;
          }
          window.setTimeout(function () {
            window.location.reload();
          }, 4000);
        });
      }
    }

    const list = document.getElementById("sheaf-chat-list");
    if (!list) return;

    const active = localStorage.getItem(ACTIVE_CHAT_KEY) || "";
    if (!Array.isArray(chats) || chats.length === 0) {
      list.innerHTML = '<div class="sheaf-empty">No chats yet</div>';
      return;
    }

    list.innerHTML = "";
    chats.forEach(function (chat) {
      const id = typeof chat.chat_id === "string" ? chat.chat_id : "";
      if (!id) return;
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "sheaf-chat-item" + (id === active ? " active" : "");
      btn.textContent = id;
      btn.title = id;
      btn.addEventListener("click", function () {
        localStorage.setItem(ACTIVE_CHAT_KEY, id);
        if (!sendWindowMessage({ type: "sheaf_switch_chat", chat_id: id })) {
          sendCommand("/use " + id);
        }
        setTimeout(loadChats, 500);
      });
      list.appendChild(btn);
    });
  }

  async function loadChats() {
    try {
      const resp = await fetch(getApiBase() + "/chats");
      if (!resp.ok) return;
      const data = await resp.json();
      renderSidebar(Array.isArray(data.chats) ? data.chats : []);
    } catch (_err) {
      // no-op
    }
  }

  function installEnterBehavior() {
    document.addEventListener(
      "keydown",
      function (event) {
        if (event.defaultPrevented || event.isComposing) return;
        if (event.key !== "Enter") return;

        const target = event.target;
        if (!(target instanceof HTMLTextAreaElement)) return;
        if (event.shiftKey) return;

        event.preventDefault();
        forceSendCurrentInput();
      },
      true
    );
  }

  function watchMessagesForActiveChat() {
    const obs = new MutationObserver(function () {
      const text = document.body ? document.body.innerText : "";
      const active = parseActiveChatFromText(text);
      if (active) localStorage.setItem(ACTIVE_CHAT_KEY, active);
    });
    obs.observe(document.body, { childList: true, subtree: true });
  }

  function boot() {
    installEnterBehavior();
    watchMessagesForActiveChat();
    loadChats();
    setInterval(loadChats, 5000);
    window.addEventListener("message", function (event) {
      const data = event.data;
      if (!data || typeof data !== "object") return;
      if (data.type === "sheaf_active_chat" && typeof data.chat_id === "string") {
        localStorage.setItem(ACTIVE_CHAT_KEY, data.chat_id);
        loadChats();
      }
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();
