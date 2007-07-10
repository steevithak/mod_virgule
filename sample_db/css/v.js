/*  mod_virgule JavaScript library
 *
 *  Copyright (C) 2007 by R. Steven Rainwater
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 */

// Global frame-buster code
if (top != self) { top.location = self.location }

// Call via onClick to clear initial field comments
function clrField(field) {
  if(field.defaultValue == field.value) field.value = "";
}

// Social Bookmarking
function sbm(e, url, title) {
 if(document.getElementById) {
  var bmbox = document.getElementById('sbm');
  bmbox.style.position="absolute";
  bmbox.style.left=e.clientX-4;
  bmbox.style.top=e.clientY+6;
  bmbox.style.display = "block";
  document.getElementById('bm_delicious').href = 'http://del.icio.us/post?url='+url+'&title='+title;
  document.getElementById('bm_digg').href = 'http://digg.com/submit?phase=2&url='+url+'&title='+title;
  document.getElementById('bm_google').href = 'http://www.google.com/bookmarks/mark?op=edit&bkmk='+url+'&title='+title;
  document.getElementById('bm_reddit').href = 'http://reddit.com/submit?url='+url+'&title='+title;
  document.getElementById('bm_stumble').href = 'http://www.stumbleupon.com/submit?url='+url+'&title='+title;
  document.getElementById('bm_newsvine').href = 'http://www.newsvine.com/_tools/seed&save?popoff=0&u='+url+'&h='+title;
  document.getElementById('bm_technorati').href = 'http://www.technorati.com/faves?add?='+url;
  document.getElementById('bm_furl').href = 'http://furl.net/storeIt.jsp?u='+url+'&t='+title;
  document.getElementById('bm_tailrank').href = 'http://tailrank.com/share/?link_href='+url+'&title='+title;
  document.getElementById('bm_simpy').href = 'http://www.simpy.com/simpy/LinkAdd.do?href='+url+'&title='+title;
 }
}

function sbmClose() {
 var bmbox = document.getElementById('sbm');
 if(bmbox) {
   bmbox.style.display = "none";
 }
}
