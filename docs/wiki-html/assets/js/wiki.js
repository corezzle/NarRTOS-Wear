/* NarRTOS Wiki — etkileşim katmanı.
   Bağımlılık yok; sayfalar dosya sisteminden (file://) de açılabilir. */

(function () {
  "use strict";

  /* ── Kod bloklarına kopyala butonu ───────────────────────────────── */
  function initCopy() {
    document.querySelectorAll("pre").forEach(function (pre) {
      if (pre.parentElement.classList.contains("codewrap")) return;
      var wrap = document.createElement("div");
      wrap.className = "codewrap";
      pre.parentNode.insertBefore(wrap, pre);
      wrap.appendChild(pre);

      var btn = document.createElement("button");
      btn.className = "copy";
      btn.type = "button";
      btn.textContent = "kopyala";
      btn.addEventListener("click", function () {
        var text = pre.innerText;
        var done = function () {
          btn.textContent = "kopyalandı";
          btn.classList.add("done");
          setTimeout(function () {
            btn.textContent = "kopyala";
            btn.classList.remove("done");
          }, 1600);
        };
        if (navigator.clipboard && navigator.clipboard.writeText) {
          navigator.clipboard.writeText(text).then(done, fallback);
        } else {
          fallback();
        }
        function fallback() {
          var ta = document.createElement("textarea");
          ta.value = text;
          ta.style.position = "fixed";
          ta.style.opacity = "0";
          document.body.appendChild(ta);
          ta.select();
          try { document.execCommand("copy"); done(); } catch (e) { /* yoksay */ }
          document.body.removeChild(ta);
        }
      });
      wrap.appendChild(btn);
    });
  }

  /* ── Sekmeler ────────────────────────────────────────────────────── */
  function initTabs() {
    document.querySelectorAll(".tabs").forEach(function (tabs) {
      var buttons = tabs.querySelectorAll(".tabbar button");
      var panels = tabs.querySelectorAll(".tabpanel");
      buttons.forEach(function (btn, i) {
        btn.setAttribute("role", "tab");
        btn.addEventListener("click", function () {
          buttons.forEach(function (b) { b.setAttribute("aria-selected", "false"); });
          panels.forEach(function (p) { p.classList.remove("on"); });
          btn.setAttribute("aria-selected", "true");
          if (panels[i]) panels[i].classList.add("on");
        });
      });
      if (buttons[0]) buttons[0].click();
    });
  }

  /* ── İçindekiler: h2'lerden üret + kaydırma takibi ───────────────── */
  function initToc() {
    var toc = document.querySelector(".toc-list");
    if (!toc) return;
    var heads = Array.prototype.slice.call(document.querySelectorAll("main h2[id]"));
    if (!heads.length) { var t = document.querySelector(".toc"); if (t) t.style.display = "none"; return; }

    heads.forEach(function (h) {
      var a = document.createElement("a");
      a.href = "#" + h.id;
      a.textContent = h.textContent;
      toc.appendChild(a);
    });

    var links = toc.querySelectorAll("a");
    function sync() {
      var y = window.scrollY + 130;
      var idx = 0;
      heads.forEach(function (h, i) { if (h.offsetTop <= y) idx = i; });
      links.forEach(function (a, i) { a.classList.toggle("active", i === idx); });
    }
    window.addEventListener("scroll", sync, { passive: true });
    sync();
  }

  /* ── Görsel büyütme ──────────────────────────────────────────────── */
  function initLightbox() {
    var box = document.createElement("div");
    box.id = "lightbox";
    var img = document.createElement("img");
    box.appendChild(img);
    document.body.appendChild(box);

    document.querySelectorAll("figure img").forEach(function (el) {
      el.addEventListener("click", function () {
        img.src = el.src;
        img.alt = el.alt || "";
        box.classList.add("on");
      });
    });
    box.addEventListener("click", function () { box.classList.remove("on"); });
    document.addEventListener("keydown", function (e) {
      if (e.key === "Escape") box.classList.remove("on");
    });
  }

  /* ── Veri dürüstlüğü demosu ──────────────────────────────────────────
     fallback null / "ipf" arasında geçiş yapınca hangi tüketicinin veriyi
     aldığını gösterir. Kural geolocation.service + telemetry.routes'tan. */
  function initIntegrityDemo() {
    var demo = document.getElementById("integrity-demo");
    if (!demo) return;

    var STATES = {
      real: {
        json: '"geo": {\n  "lat": 41.0289898, "lng": 29.2229154,\n  "accuracy_m": 18.9,\n  "fallback": null,\n  "provider": "www.googleapis.com"\n}',
        sinks: { coords: true, queue: true, panel: true },
        panelNote: "konum olarak, OSM bağlantısıyla",
      },
      ipf: {
        json: '"geo": {\n  "lat": 39.9207000, "lng": 32.8541000,\n  "accuracy_m": 25000,\n  "fallback": "ipf",\n  "provider": "www.googleapis.com"\n}',
        sinks: { coords: false, queue: false, panel: true },
        panelNote: "yalnızca gri bilgi olarak",
      },
    };

    var out = demo.querySelector(".demo-json code");
    var buttons = demo.querySelectorAll(".switch button");
    var sinks = {
      coords: demo.querySelector('[data-sink="coords"]'),
      queue: demo.querySelector('[data-sink="queue"]'),
      panel: demo.querySelector('[data-sink="panel"]'),
    };

    function apply(key) {
      var s = STATES[key];
      out.textContent = s.json;
      Object.keys(sinks).forEach(function (k) {
        var el = sinks[k];
        if (!el) return;
        var pass = s.sinks[k];
        el.classList.toggle("pass", pass);
        el.classList.toggle("block", !pass);
        el.querySelector(".sink-v").textContent = pass ? "✓" : "✕";
        if (k === "panel") el.querySelector(".sink-s").textContent = s.panelNote;
      });
      buttons.forEach(function (b) {
        b.setAttribute("aria-pressed", String(b.dataset.state === key));
      });
    }

    buttons.forEach(function (b) {
      b.addEventListener("click", function () { apply(b.dataset.state); });
    });
    apply("real");
  }

  document.addEventListener("DOMContentLoaded", function () {
    initCopy();
    initTabs();
    initToc();
    initLightbox();
    initIntegrityDemo();
  });
})();
