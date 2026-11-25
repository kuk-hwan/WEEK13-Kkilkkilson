"use strict";

function show(label, response, text) {
  const status = document.getElementById("status");
  status.textContent = label + " → " + response.status + " " + response.statusText;
  status.className = response.ok ? "ok" : "error";
  document.getElementById("body").textContent = text;
}

async function request(label, url, options) {
  try {
    const response = await fetch(url, options);
    show(label, response, await response.text());
  } catch (error) {
    const status = document.getElementById("status");
    status.textContent = label + " → 요청 실패";
    status.className = "error";
    document.getElementById("body").textContent = String(error);
  }
}

document.getElementById("whoami").addEventListener("click", () => {
  request("GET /api/whoami", "/api/whoami", { cache: "no-store" });
});

document.getElementById("echo").addEventListener("click", () => {
  request("POST /api/echo", "/api/echo", {
    method: "POST",
    headers: { "Content-Type": "text/plain; charset=utf-8" },
    body: document.getElementById("text").value,
  });
});

document.getElementById("missing").addEventListener("click", () => {
  request("GET /missing.html", "/missing.html", { cache: "no-store" });
});
