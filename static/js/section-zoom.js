// Heading-level "zoom"/focus mode for the document view page: click a
// small control next to any h2-h6 and everything in the document body
// OUTSIDE that heading's own section (its content up to the next
// heading of the same or shallower level) gets hidden. A lightweight,
// client-side-only stand-in for true block-level zoom (SiYuan/Notion/
// Roam/LogSeq) -- this app's file-based (not block-based) content model
// can't offer that without a real architecture change (see
// docs/architecture.md), but a heading-scoped version costs nothing:
// pages/view.js's rendered HTML already has real h2-h6 structure, this
// file just hides/shows siblings around it.
//
// h1 excluded on purpose: a document's body conventionally opens with
// its own "# Title" h1 (see pages/view.js's own bodyHasOwnH1 comment)
// -- that already IS "the whole document", so there's nothing to zoom
// OUT of it into.
//
// Every heading also gets a stable `id` (a slugified version of its own
// text, de-duplicated per page) as a plain HTML anchor -- useful on its
// own even without zoom (a real #section link, browser-native
// scroll-to-anchor). The zoom STATE itself lives in a separate hash
// shape, `#zoom=<slug>`, so a bare `#slug` link (scroll only, standard
// browser behavior, needs nothing from this file) and a `#zoom=slug`
// one (scroll AND hide everything else) stay distinguishable.
window.WikiSectionZoom = (function () {
  "use strict";

  function slugify(text) {
    var slug = text
      .toLowerCase()
      .trim()
      .replace(/[^a-z0-9]+/g, "-")
      .replace(/^-+|-+$/g, "");
    return slug || "section";
  }

  function headingLevel(el) {
    return parseInt(el.tagName.charAt(1), 10);
  }

  // Direct children of `body` only -- real markdown never nests a
  // heading inside a blockquote/list/table, so this covers every
  // practical case, not a simplification that quietly drops real
  // content.
  function headingsIn(body) {
    return Array.prototype.slice.call(
      body.querySelectorAll(":scope > h2, :scope > h3, :scope > h4, :scope > h5, :scope > h6")
    );
  }

  // Every direct child of `body` between `heading` (exclusive) and the
  // next heading whose level is <= heading's own (exclusive) -- exactly
  // this section's own content, correctly keeping a DEEPER subsection's
  // own heading (an h3 living under this h2) inside the zoom instead of
  // cutting it off.
  function sectionElements(heading) {
    var level = headingLevel(heading);
    var out = [];
    var node = heading.nextElementSibling;
    while (node) {
      if (/^H[2-6]$/.test(node.tagName) && headingLevel(node) <= level) break;
      out.push(node);
      node = node.nextElementSibling;
    }
    return out;
  }

  function applyZoom(body, banner, heading) {
    var keep = [heading].concat(sectionElements(heading));
    Array.prototype.forEach.call(body.children, function (el) {
      if (el === banner) return;
      el.hidden = keep.indexOf(el) === -1;
    });
    banner.hidden = false;
    // heading.textContent, not the raw DOM property here -- by this
    // point the heading has its own "Zoom" button appended as a CHILD
    // (see setup() below), and textContent walks every descendant's
    // text, button label included -- confirmed live, the banner first
    // read "shared_ptrZoom" before this was caught. dataset.zoomLabel is
    // the heading's own text captured in setup(), BEFORE that button
    // existed.
    banner.querySelector(".zoom-current").textContent = heading.dataset.zoomLabel;
    // replaceState, not a bare location.hash assignment -- the latter
    // pushes a new history entry, meaning the browser Back button would
    // step through every zoom click instead of leaving the page. Zoom is
    // view state, not navigation.
    history.replaceState(null, "", location.pathname + location.search + "#zoom=" + heading.id);
  }

  function clearZoom(body, banner) {
    Array.prototype.forEach.call(body.children, function (el) {
      el.hidden = false;
    });
    banner.hidden = true;
    history.replaceState(null, "", location.pathname + location.search);
  }

  // Wires zoom controls into `container` -- the document VIEW page's own
  // content container, same as WikiMermaid.renderIn/WikiPrismHighlight.
  // No-op if the document has no h2+ headings to zoom into at all.
  function setup(container) {
    var body = container.querySelector("#doc-body");
    if (!body) return;
    var headings = headingsIn(body);
    if (headings.length === 0) return;

    var usedSlugs = {};
    headings.forEach(function (h) {
      // Captured BEFORE the "Zoom" button below becomes a child of `h`
      // -- textContent walks every descendant's text, so reading it
      // again later (applyZoom's banner label) would include the
      // button's own "Zoom" label glued onto the real heading text.
      h.dataset.zoomLabel = h.textContent;

      var base = slugify(h.textContent);
      var slug = base;
      var n = 2;
      while (usedSlugs[slug]) {
        slug = base + "-" + n++;
      }
      usedSlugs[slug] = true;
      h.id = slug;

      var btn = document.createElement("button");
      btn.type = "button";
      btn.className = "zoom-btn";
      btn.title = "Focus on this section";
      btn.textContent = "Zoom";
      btn.addEventListener("click", function () {
        applyZoom(body, banner, h);
      });
      h.appendChild(btn);
    });

    var banner = document.createElement("div");
    banner.className = "zoom-banner";
    banner.hidden = true;
    banner.innerHTML =
      '<button type="button" class="zoom-exit">&larr; Show full document</button> ' +
      "Focused on: <strong class=\"zoom-current\"></strong>";
    body.insertBefore(banner, body.firstChild);
    banner.querySelector(".zoom-exit").addEventListener("click", function () {
      clearZoom(body, banner);
    });

    // Restore on load: a shared "#zoom=<slug>" link lands already
    // focused, not requiring a second click to reproduce what was
    // shared.
    var match = /^#zoom=(.+)$/.exec(location.hash);
    if (match) {
      var target = document.getElementById(decodeURIComponent(match[1]));
      if (target && headings.indexOf(target) !== -1) {
        applyZoom(body, banner, target);
      }
    }
  }

  return { setup: setup };
})();
