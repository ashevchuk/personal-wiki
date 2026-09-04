// Line-based diff (classic LCS dynamic-programming, not Myers) — used by
// pages/history.js to compare a past snapshot's body against another
// snapshot's or the live document's. No vendored diff library anywhere
// in this project (see static/js/toastui-editor/, static/js/htmx/ for
// what IS vendored) — this is small and self-contained enough not to
// need one.
//
// O(n*m) time AND space (a full (n+1)x(m+1) table) — fine for ordinary
// markdown document sizes (hundreds to low thousands of lines); a
// genuinely huge document (tens of thousands of lines on both sides)
// would mean a multi-hundred-MB table, but that's not a shape a
// personal-wiki document is expected to take, and this is a manual,
// admin-triggered action (viewing history), not a hot path.
window.WikiDiff = (function () {
  "use strict";

  // Returns an array of {type: "same"|"add"|"remove", line: string}, in
  // order, describing how to turn oldText into newText one line at a
  // time. "same" lines appear once (not duplicated as both a remove and
  // an add), matching standard unified-diff behavior.
  function diffLines(oldText, newText) {
    var a = (oldText || "").split("\n");
    var b = (newText || "").split("\n");
    var n = a.length;
    var m = b.length;

    // dp[i][j] = length of the LCS of a[i:] and b[j:].
    var dp = new Array(n + 1);
    for (var i = 0; i <= n; i++) {
      dp[i] = new Array(m + 1).fill(0);
    }
    for (i = n - 1; i >= 0; i--) {
      for (var j = m - 1; j >= 0; j--) {
        dp[i][j] = a[i] === b[j] ? dp[i + 1][j + 1] + 1 : Math.max(dp[i + 1][j], dp[i][j + 1]);
      }
    }

    var result = [];
    i = 0;
    var jj = 0;
    while (i < n && jj < m) {
      if (a[i] === b[jj]) {
        result.push({ type: "same", line: a[i] });
        i++;
        jj++;
      } else if (dp[i + 1][jj] >= dp[i][jj + 1]) {
        result.push({ type: "remove", line: a[i] });
        i++;
      } else {
        result.push({ type: "add", line: b[jj] });
        jj++;
      }
    }
    while (i < n) {
      result.push({ type: "remove", line: a[i] });
      i++;
    }
    while (jj < m) {
      result.push({ type: "add", line: b[jj] });
      jj++;
    }
    return result;
  }

  return { diffLines: diffLines };
})();
