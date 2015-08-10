This is just a lua binding to the small, portable xyssl library in order to provide SSL functionalities in lua. It needs luasocket for the socket creation (bind, accept, connect etc).

In other words, it is a "SSL filter" for an existing socket(or pipe including stdin/stdout say launching from inetd).

===Status Update === (06/08/2010)
  * Please use SVN trunk as I would no longer create downloadable package
  * This project will not have future activity other than user supplied patches

===version 0.2=== (04/11/2007)

  * now support xyssl 0.7 and xyssl 0.8
  * now support building on Windows using VC(including VC 8)
  * source tarball can be found in the download area
  * Windows binaries can be found in the download area(compatible with lua binaries 5.1)
  * the source code of xyssl 0.7 and xyssl 0.8 can also be found in the download area

===version 0.1=== (04/11/2007)

  * plug replacement of lua socket read/write(through the associated bufferio.lua)
  * COPAS compatible(see httpd-co.lua)
  * binding for cipher AES(ECB, CBC, CFB) and RC4
  * binding for MD5/SHA1 and assoicated HMAC
  * binding for a fast crypto grade HAVEGE pseudo random generator


more information about lua can be found at http://www.lua.org
more information about xyssl can be found at http://xyssl.org