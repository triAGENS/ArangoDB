(window.webpackJsonp=window.webpackJsonp||[]).push([[9],{467:function(e,i){(function(){!function(){"use strict";sigma.utils.pkg("sigma.canvas.edges"),sigma.canvas.edges.tapered=function(e,i,o,l,a){var t=e.active?e.active_color||a("defaultEdgeActiveColor"):e.color,c=e[(r=a("prefix")||"")+"size"]||1,s=a("edgeColor"),r=a("prefix")||"",n=a("defaultNodeColor"),g=a("defaultEdgeColor"),d=i[r+"x"],f=i[r+"y"],u=o[r+"x"],p=o[r+"y"],v=sigma.utils.getDistance(d,f,u,p);if(!t)switch(s){case"source":t=i.color||n;break;case"target":t=o.color||n;break;default:t=g}var w=sigma.utils.getCircleIntersection(d,f,c,u,p,v);l.save(),e.active?l.fillStyle="edge"===a("edgeActiveColor")?t||g:a("defaultEdgeActiveColor"):l.fillStyle=t,l.globalAlpha=.65,l.beginPath(),l.moveTo(u,p),l.lineTo(w.xi,w.yi),l.lineTo(w.xi_prime,w.yi_prime),l.closePath(),l.fill(),l.restore()}}()}).call(window)}}]);
//# sourceMappingURL=9.66d27d00.chunk.js.map