(function () {
  var reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  var nav = document.querySelector("header.site");
  var tracks = document.querySelectorAll(".scrolly-track");
  var ticking = false;

  function update() {
    ticking = false;
    if (nav && !document.body.classList.contains("page")) {
      nav.classList.toggle("solid", window.scrollY > window.innerHeight * 0.7);
    }
    tracks.forEach(function (track) {
      var stage = track.querySelector(".scrolly-stage");
      if (!stage) return;
      var rect = track.getBoundingClientRect();
      var total = track.offsetHeight - window.innerHeight;
      var progress = total > 0 ? -rect.top / total : 0;
      progress = Math.min(1, Math.max(0, progress));
      var steps = stage.querySelectorAll(".scrolly-step").length || 1;
      var stepIndex = Math.min(steps - 1, Math.floor(progress * steps));
      stage.setAttribute("data-active", String(stepIndex));
    });
  }

  function onScroll() {
    if (!ticking) {
      ticking = true;
      requestAnimationFrame(update);
    }
  }

  document.addEventListener("scroll", onScroll, { passive: true });
  window.addEventListener("resize", onScroll);
  update();

  // fade-up reveals (content pages)
  var revealEls = document.querySelectorAll(".reveal");
  if (revealEls.length) {
    if (reduceMotion || !("IntersectionObserver" in window)) {
      revealEls.forEach(function (el) { el.classList.add("is-visible"); });
    } else {
      var io = new IntersectionObserver(
        function (entries) {
          entries.forEach(function (entry) {
            if (entry.isIntersecting) {
              entry.target.classList.add("is-visible");
              io.unobserve(entry.target);
            }
          });
        },
        { threshold: 0.12, rootMargin: "0px 0px -40px 0px" }
      );
      revealEls.forEach(function (el) { io.observe(el); });
    }
  }

  // count-up numbers
  var stats = document.querySelectorAll(".stat .n[data-count]");
  if (stats.length) {
    var animate = function (el) {
      var target = parseInt(el.getAttribute("data-count"), 10);
      var suffix = el.getAttribute("data-suffix") || "";
      if (reduceMotion) { el.textContent = target + suffix; return; }
      var start = null;
      var duration = 1100;
      function step(ts) {
        if (start === null) start = ts;
        var t = Math.min(1, (ts - start) / duration);
        var eased = 1 - Math.pow(1 - t, 3);
        el.textContent = Math.round(eased * target) + suffix;
        if (t < 1) requestAnimationFrame(step);
      }
      requestAnimationFrame(step);
    };
    if ("IntersectionObserver" in window) {
      var sio = new IntersectionObserver(
        function (entries) {
          entries.forEach(function (entry) {
            if (entry.isIntersecting) { animate(entry.target); sio.unobserve(entry.target); }
          });
        },
        { threshold: 0.6 }
      );
      stats.forEach(function (el) { sio.observe(el); });
    } else {
      stats.forEach(animate);
    }
  }
})();
