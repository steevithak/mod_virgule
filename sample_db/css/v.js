/*  mod_virgule JavaScript library
 *
 *  Copyright (C) 2007 by R. Steven Rainwater
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

// Call via onClick to clear initial field comments
function clrField(field) {
  if(field.defaultValue == field.value) field.value = "";
}
