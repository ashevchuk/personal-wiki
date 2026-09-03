// Canvas "digital rain" background, purely decorative — matches the
// green-on-black terminal theme (theme.css). Self-contained, no deps.
// Skips entirely under prefers-reduced-motion (theme.css also hides the
// canvas in that case, but avoid burning CPU on a hidden canvas too).
(function () {
  "use strict";

  var canvas = document.getElementById("matrix-bg");
  if (!canvas) return;

  var reduceMotion =
    window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  if (reduceMotion) return;

  var ctx = canvas.getContext("2d");
  var fontSize = 16;
  var columns = 0;
  var drops = [];

  var chars =
    "アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン0123456789";

  function resize() {
    canvas.width = window.innerWidth;
    canvas.height = window.innerHeight;
    columns = Math.floor(canvas.width / fontSize);
    var oldDrops = drops;
    drops = new Array(columns);
    for (var i = 0; i < columns; i++) {
      drops[i] = oldDrops[i] !== undefined ? oldDrops[i] : Math.random() * canvas.height / fontSize;
    }
  }

  function draw() {
    ctx.fillStyle = "rgba(0, 0, 0, 0.08)";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = "#00ff00";
    ctx.font = fontSize + "px monospace";
    for (var i = 0; i < drops.length; i++) {
      var ch = chars.charAt(Math.floor(Math.random() * chars.length));
      ctx.fillText(ch, i * fontSize, drops[i] * fontSize);
      if (drops[i] * fontSize > canvas.height && Math.random() > 0.975) {
        drops[i] = 0;
      }
      drops[i]++;
    }
  }

  window.addEventListener("resize", resize);
  resize();
  setInterval(draw, 40);
})();
