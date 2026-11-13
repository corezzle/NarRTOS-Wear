/* Terminal senaryosunu DOM'a ve tek bir duraklatılmış GSAP zaman çizelgesine çevirir.
   Adımlar:  {type:"cmd", text, pause?, speed?, enter?}   → "$ " + harf harf yazım
             {type:"out", text, cls?, dt?}                → çıktı satırı, dt sonra sıradaki
             {type:"wait", dt}
   Görünür satır sayısı aşılınca #lines yukarı kayar (transform).
   Süre: kök data-duration'dan okunur; içerik ondan 0.6 sn önce solar → GIF temiz döner. */
window.buildTerminal = function (id, steps, opts) {
  opts = opts || {};
  var LH = opts.lineHeight || 22, VISIBLE = opts.visible || 14;
  var root = document.getElementById("root");
  var D = parseFloat(root.getAttribute("data-duration"));
  var lines = document.getElementById("lines");
  var tl = gsap.timeline({ paused: true });
  window.__timelines = window.__timelines || {};
  window.__timelines[id] = tl;

  function esc(s) { return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }
  var t = opts.start != null ? opts.start : 0.6, count = 0;
  function addLine(html, cls) {
    var el = document.createElement("span");
    el.className = "ln" + (cls ? " " + cls : "");
    el.innerHTML = html;
    lines.appendChild(el);
    return el;
  }
  function show(el, at) {
    tl.set(el, { opacity: 1 }, at);
    count++;
    /* Kaydırma anlık: ara kareler tüm ekranı yeniden çizdirip GIF'i şişiriyordu. */
    if (count > VISIBLE) tl.set(lines, { y: -(count - VISIBLE) * LH }, at);
  }
  steps.forEach(function (s) {
    if (s.type === "cmd") {
      var el = addLine('<span class="p">$ </span>');
      var chars = [];
      Array.prototype.forEach.call(s.text, function (ch) {
        var sp = document.createElement("span"); sp.className = "c ch"; sp.textContent = ch;
        el.appendChild(sp); chars.push(sp);
      });
      var cur = document.createElement("span"); cur.className = "cur"; el.appendChild(cur);
      show(el, t);
      tl.set(cur, { opacity: 1 }, t);
      var pause = s.pause != null ? s.pause : 0.7, speed = s.speed || 0.045;
      tl.to(chars, { opacity: 1, duration: 0.01, stagger: speed }, t + pause);
      t += pause + speed * chars.length + (s.enter != null ? s.enter : 0.45);
      tl.set(cur, { opacity: 0 }, t);
    } else if (s.type === "out") {
      show(addLine(esc(s.text), s.cls || "o"), t);
      t += s.dt != null ? s.dt : 0.12;
    } else if (s.type === "wait") {
      t += s.dt;
    }
  });
  if (t > D - 1.0) console.warn("terminal: içerik " + t.toFixed(2) + " sn'de bitiyor, data-duration=" + D + " kısa");
  // GIF döngüsü için içerik sonda solar; video sürümlerinde (holdEnd) çıktı ekranda kalır.
  if (!opts.holdEnd) tl.to(lines, { opacity: 0, duration: 0.5 }, D - 0.6);
  return t;
};
