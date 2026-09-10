// Auto-hiding scrollbars: the thumb stays transparent (see the
// scrollbar-color/::-webkit-scrollbar-thumb rules in each
// css/themes/*.css file) until either a scroll actually happens or the
// pointer hovers the scrollbar's own track area (that second part is
// pure CSS, :hover on ::-webkit-scrollbar-thumb/scrollbar-color still
// fires even while the thumb color is transparent — the track itself
// still occupies its width, just paints nothing). This file exists
// ONLY for the "reveal while a scroll is actually in flight" half —
// CSS has no :is-scrolling pseudo-class, so that part needs JS.
//
// Global at the document level via a capturing listener: scroll events
// on a specific overflow:auto element (e.g. .sidebar-scroll) do NOT
// bubble in the standard DOM, but a capture-phase listener on document
// still sees every one of them on the way down, regardless of which
// element down the tree the scroll happened on — one listener covers
// window scroll AND every scrollable region without needing to know
// their selectors up front.
(function () {
  "use strict";

  var HIDE_DELAY_MS = 600; // fades back out this long after the last scroll event
  var hideTimers = new WeakMap(); // per-element timeout handle, so scrolling two regions at once doesn't cancel each other's timer early

  function targetElement(evt) {
    // window/document-level scroll reports evt.target as the document;
    // the class has to land on <html> since that's the element the
    // scrollbar rules in css/themes/*.css actually match for the page's
    // own scrollbar.
    return evt.target === document ? document.documentElement : evt.target;
  }

  function onScroll(evt) {
    var el = targetElement(evt);
    if (!el || typeof el.classList === "undefined") return;

    el.classList.add("is-scrolling");

    var existing = hideTimers.get(el);
    if (existing) clearTimeout(existing);
    hideTimers.set(el, setTimeout(function () {
      el.classList.remove("is-scrolling");
      hideTimers.delete(el);
    }, HIDE_DELAY_MS));
  }

  document.addEventListener("scroll", onScroll, { capture: true, passive: true });
})();
