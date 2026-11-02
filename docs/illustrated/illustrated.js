/* Shared helpers for docs/illustrated/*.html. No dependencies. */
(function () {
  var PAGES = [
    ['index.html', 'Overview'], ['cluster.html', 'Cluster'], ['data-path.html', 'Data path'],
    ['metadata.html', 'Metadata'], ['space.html', 'Space'], ['sessions.html', 'Sessions'],
    ['failure.html', 'Failures'], ['operate.html', 'Operate']
  ];
  var here = location.pathname.split('/').pop() || 'index.html';
  var h = document.querySelector('header.site');
  if (h) {
    var nav = document.createElement('nav');
    nav.setAttribute('aria-label', 'Illustrated EFS pages');
    var brand = document.createElement('a');
    brand.className = 'brand'; brand.href = 'index.html'; brand.textContent = 'EFS illustrated';
    nav.appendChild(brand);
    PAGES.forEach(function (p) {
      var a = document.createElement('a');
      a.className = 'nav'; a.href = p[0]; a.textContent = p[1];
      if (p[0] === here) a.setAttribute('aria-current', 'page');
      nav.appendChild(a);
    });
    h.appendChild(nav);
  }
  Array.prototype.forEach.call(document.querySelectorAll('svg.fig'), function (g) {
    var w = document.createElement('div'); w.className = 'figwrap'; w.tabIndex = 0; w.setAttribute('role', 'region'); w.setAttribute('aria-label', (g.getAttribute('aria-label') || 'Diagram') + ' — scroll horizontally on narrow screens');
    g.parentNode.insertBefore(w, g); w.appendChild(g);
  });
  var main = document.querySelector('main');
  if (main) {
    main.id = main.id || 'main';
    var skip=document.createElement('a'); skip.href='#'+main.id; skip.className='skip-link'; skip.textContent='Skip to content'; document.body.insertBefore(skip,document.body.firstChild);
    var toc=document.createElement('nav'); toc.className='section-nav'; toc.setAttribute('aria-label','On this page');
    var heading=document.createElement('strong'); heading.textContent='On this page'; toc.appendChild(heading);
    main.querySelectorAll('h2').forEach(function(h,i) { h.id=h.id || 'section-'+(i+1); var a=document.createElement('a'); a.href='#'+h.id; a.textContent=h.textContent; toc.appendChild(a); });
    var lead=main.querySelector('.lead'); if(lead) lead.after(toc);
    var pager=document.createElement('nav'); pager.className='topic-nav'; pager.setAttribute('aria-label','Previous and next topic');
    var ix=PAGES.findIndex(function(p){return p[0]===here;});
    [ix-1,ix+1].forEach(function(i){if(i>=0 && i<PAGES.length){var a=document.createElement('a');a.href=PAGES[i][0];a.textContent=(i<ix?'← Previous: ':'Next: ')+PAGES[i][1];pager.appendChild(a);}}); main.appendChild(pager);
  }
  // Colour outcome words in log lines: red for failures, green for success, amber for degraded.
  var HL=[['bad',/\b(failed|fails?|refused|refuses|cannot|EIO|ENOSPC|NOT_PRIMARY|no majority|no quorum|unavailable|unrecoverable|stale|fenced|blocked|lost|down|corrupt\w*|bad sum)\b/gi],
          ['warn',/\b(degraded)\b/gi],
          ['ok',/\b(published|fully protected|reconstructed|rebuilt|repaired|restored|succeeds|succeeded|elected|OK)\b/g]];
  function highlight(root){
    var walker=document.createTreeWalker(root,NodeFilter.SHOW_TEXT),nodes=[],t;
    while((t=walker.nextNode()))if(!t.parentElement.closest('.w-bad,.w-ok,.w-warn,script,style,svg,code,pre,h1,h2,h3,h4,button,select,option,textarea,input,nav,summary,a,label,footer,.latest-outcome'))nodes.push(t);
    nodes.forEach(function(node){
      var text=node.nodeValue,marks=[];
      HL.forEach(function(h){h[1].lastIndex=0;var m;while((m=h[1].exec(text)))marks.push([m.index,m.index+m[0].length,h[0]]);});
      if(!marks.length)return;
      marks.sort(function(x,y){return x[0]-y[0];});
      var frag=document.createDocumentFragment(),pos=0;
      marks.forEach(function(m){
        if(m[0]<pos)return;
        frag.append(text.slice(pos,m[0]));
        var sp=document.createElement('span');sp.className='w-'+m[2];sp.textContent=text.slice(m[0],m[1]);frag.append(sp);pos=m[1];
      });
      frag.append(text.slice(pos));node.replaceWith(frag);
    });
  }
  var main0=document.querySelector('main')||document.body,pending=0;
  function sweep(){pending=0;highlight(main0);}
  new MutationObserver(function(){if(!pending)pending=setTimeout(sweep,50);}).observe(main0,{childList:true,subtree:true,characterData:true});
  // Keep the newest outcome visible while history stays bounded and optional.
  document.querySelectorAll('.log').forEach(function(log) {
    var details=document.createElement('details'), title=document.createElement('summary'), latest=document.createElement('div');
    details.className='event-history'; title.textContent='Event history'; latest.className='latest-outcome'; latest.setAttribute('role','status');
    log.before(latest,details); details.append(title,log); log.removeAttribute('aria-live');
    function update(){
      Array.prototype.forEach.call(log.children,function(p){if(!p.dataset.hl){p.dataset.hl='1';highlight(p);}});
      if(log.lastElementChild){latest.innerHTML=log.lastElementChild.innerHTML;}else{latest.textContent='Ready.';}
    }
    new MutationObserver(update).observe(log,{childList:true,subtree:true,characterData:true}); update();
  });
  document.querySelectorAll('.figwrap').forEach(function(w) {
    var hint=document.createElement('p');hint.className='diagram-hint';hint.textContent='On a narrow screen, scroll this diagram horizontally. A text summary follows.';w.before(hint);
    var details=document.createElement('details'), summary=document.createElement('summary'), text=document.createElement('p');summary.textContent='Diagram text summary';details.className='diagram-summary';details.append(summary,text);w.after(details);
    function update(){text.textContent=Array.from(w.querySelectorAll('svg text')).map(function(t){return t.textContent;}).filter(Boolean).join(' · ');}
    new MutationObserver(update).observe(w,{childList:true,subtree:true,characterData:true});update();
  });
  var NS = 'http://www.w3.org/2000/svg';
  /* svg('rect', {x: 1}, parent, 'text') */
  window.svg = function (tag, attrs, parent, text) {
    var e = document.createElementNS(NS, tag);
    for (var k in attrs || {}) e.setAttribute(k, attrs[k]);
    if (text != null) e.textContent = text;
    if (parent) parent.appendChild(e);
    return e;
  };
  window.$ = function (id) { return document.getElementById(id); };
  /* Illustrative stable hash, NOT the product's placement hash. */
  window.mix = function (a, b) {
    var x = (a * 2654435761 + b * 40503 + 12345) >>> 0;
    x ^= x >>> 15; x = Math.imul(x, 2246822519) >>> 0; x ^= x >>> 13;
    return x >>> 0;
  };
  /* Three distinct node ids (1..n) for a chunk: spaced around the ring, start hashed from the file. */
  window.placeChunk = function (n, file, chunk) {
    var start = (window.mix(file, 7) + chunk) % n, step = Math.max(1, Math.floor(n / 3));
    var out = [], i = 0;
    while (out.length < 3) {
      var id = ((start + i * step) % n) + 1;
      if (out.indexOf(id) < 0) out.push(id); else { start++; i = -1; out = []; }
      i++;
    }
    return out;
  };
  window.voters = function (group, n) {
    if (n <= 3) return [1, 2, 3].slice(0, n);
    return group === 0 ? [1, 2, 3] : [2, 3, 4];
  };
  var f = document.querySelector('footer.site');
  if (f && !f.textContent.trim()) {
    f.innerHTML = 'Teaching aid drawn from <code>docs/how-it-works/architecture.md</code>, ' +
      '<code>docs/using/alpha-1.0-guide.md</code> and the code on devel. It is not a qualification claim; ' +
      'the sources win where they differ. Diagram placement and timing are illustrative.';
  }
  sweep();
})();
