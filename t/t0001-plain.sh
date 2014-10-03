#!/bin/sh
#
# Copyright (C) 2014 by Maxim Bublis <b@codemonkey.ru>
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

test_description='Test plain history support'

. ./helpers.sh
. ./lib/test.sh

test_export_import() {
	test_expect_success 'Import dump into Git' '
	svnadmin dump repo >repo.dump &&
		(cd repo.git && git-svn-fast-import <../repo.dump)
	'
}

test_expect_success 'Initialize repositories' '
svnadmin create repo &&
	echo "#!/bin/sh" >repo/hooks/pre-revprop-change &&
	chmod +x repo/hooks/pre-revprop-change &&
	svn checkout file:///$(pwd)/repo repo.svn &&
	git init repo.git
'

cat >repo.svn/main.c <<EOF
int main() {
	return 0;
}
EOF

test_tick

test_expect_success 'Commit new file' '
(cd repo.svn &&
	svn add main.c &&
	svn commit -m "Initial revision" &&
	svn propset svn:date --revprop -r HEAD $COMMIT_DATE &&
	svn propset svn:author --revprop -r HEAD author1)
'

test_export_import

cat >expect <<EOF
bcd5f99c825242e10b409f125f49d7cc931d55bc
:000000 100644 0000000000000000000000000000000000000000 cb3f7482fa46d2ac25648a694127f23c1976b696 A	main.c
EOF

test_expect_success 'Validate files added' '
(cd repo.git &&
	git diff-tree --root master >actual &&
	test_cmp ../expect actual)
'

cat >repo.svn/main.c <<EOF
#include <stdio.h>

int main() {
	printf("Hello, world\n");
	return 0;
}
EOF

test_tick

test_expect_success 'Commit file modification' '
(cd repo.svn &&
	svn commit -m "Some modification" &&
	svn propset svn:date --revprop -r HEAD $COMMIT_DATE &&
	svn propset svn:author --revprop -r HEAD author1)
'

test_export_import

cat >expect <<EOF
:100644 100644 cb3f7482fa46d2ac25648a694127f23c1976b696 0e5f181f94f2ff9f984b4807887c4d2c6f642723 M	main.c
EOF

test_expect_success 'Validate files modified' '
(cd repo.git &&
	git diff-tree -M -r master^ master >actual &&
	test_cmp ../expect actual)
'

test_tick

mkdir -p repo.svn/lib
cat >repo.svn/lib.c <<EOF
void dummy(void) {
	// Do nothing
}
EOF

test_expect_success 'Commit empty dir and new file' '
(cd repo.svn &&
	svn add lib &&
	svn add lib.c &&
	svn commit -m "Empty dir added" &&
	svn propset svn:date --revprop -r HEAD $COMMIT_DATE &&
	svn propset svn:author --revprop -r HEAD author1)
'

test_export_import

cat >expect <<EOF
:000000 100644 0000000000000000000000000000000000000000 87734a8c690949471d1836b2e9247ad8f82c9df6 A	lib.c
EOF

test_expect_success 'Validate empty dir was not added' '
(cd repo.git &&
	git diff-tree -M -r master^ master >actual &&
	test_cmp ../expect actual)
'

test_tick

test_expect_success 'Commit file move' '
(cd repo.svn &&
	svn mv lib.c lib &&
	svn commit -m "File moved to dir" &&
	svn propset svn:date --revprop -r HEAD $COMMIT_DATE &&
	svn propset svn:author --revprop -r HEAD author1)
'

test_export_import

cat >expect <<EOF
:100644 100644 87734a8c690949471d1836b2e9247ad8f82c9df6 87734a8c690949471d1836b2e9247ad8f82c9df6 R100	lib.c	lib/lib.c
EOF

test_expect_success 'Validate file move' '
(cd repo.git &&
	git diff-tree -M -r master^ master >actual &&
	test_cmp ../expect actual)
'

test_tick

test_expect_success 'Commit file copy' '
(cd repo.svn &&
	svn cp main.c lib &&
	svn commit -m "File copied to dir" &&
	svn propset svn:date --revprop -r HEAD $COMMIT_DATE &&
	svn propset svn:author --revprop -r HEAD author1)
'

test_export_import

cat >expect <<EOF
:000000 100644 0000000000000000000000000000000000000000 0e5f181f94f2ff9f984b4807887c4d2c6f642723 A	lib/main.c
EOF

test_expect_success 'Validate file copy' '
(cd repo.git &&
	git diff-tree -M -r master^ master >actual &&
	test_cmp ../expect actual)
'

test_tick

test_expect_success 'Commit file delete' '
(cd repo.svn &&
	svn rm main.c &&
	svn commit -m "File removed" &&
	svn propset svn:date --revprop -r HEAD $COMMIT_DATE &&
	svn propset svn:author --revprop -r HEAD author1)
'

test_export_import

cat >expect <<EOF
:100644 000000 0e5f181f94f2ff9f984b4807887c4d2c6f642723 0000000000000000000000000000000000000000 D	main.c
EOF

test_expect_success 'Validate file remove' '
(cd repo.git &&
	git diff-tree master^ master >actual &&
	test_cmp ../expect actual)
'

test_tick

test_expect_success 'Commit directory move' '
(cd repo.svn &&
	svn update &&
	svn mv lib src &&
	svn commit -m "Directory renamed" &&
	svn propset svn:date --revprop -r HEAD $COMMIT_DATE &&
	svn propset svn:author --revprop -r HEAD author1)
'

test_export_import

cat >expect <<EOF
:100644 100644 87734a8c690949471d1836b2e9247ad8f82c9df6 87734a8c690949471d1836b2e9247ad8f82c9df6 R100	lib/lib.c	src/lib.c
:100644 100644 0e5f181f94f2ff9f984b4807887c4d2c6f642723 0e5f181f94f2ff9f984b4807887c4d2c6f642723 R100	lib/main.c	src/main.c
EOF

test_expect_failure 'Validate directory move' '
(cd repo.git &&
	git diff-tree -M -r master^ master >actual &&
	test_cmp ../expect actual)
'

test_tick

test_expect_success 'Commit directory copy' '
(cd repo.svn &&
	svn cp src lib &&
	svn commit -m "Directory copied" &&
	svn propset svn:date --revprop -r HEAD $COMMIT_DATE &&
	svn propset svn:author --revprop -r HEAD author1)
'

test_export_import

cat >expect <<EOF
:040000 040000 f9e724c547c90dfac10d779fcae9f9bc299245c1 f9e724c547c90dfac10d779fcae9f9bc299245c1 C100	src	lib
EOF

test_expect_failure 'Validate directory copy' '
(cd repo.git &&
	git diff-tree --find-copies-harder master^ master >actual &&
	test_cmp ../expect actual)
'

test_tick

test_expect_success 'Commit directory delete' '
(cd repo.svn &&
	svn rm src &&
	svn commit -m "Directory removed" &&
	svn propset svn:date --revprop -r HEAD $COMMIT_DATE &&
	svn propset svn:author --revprop -r HEAD author1)
'

test_export_import

cat >expect <<EOF
:040000 000000 f9e724c547c90dfac10d779fcae9f9bc299245c1 0000000000000000000000000000000000000000 D	src
EOF

test_expect_failure 'Validate directory remove' '
(cd repo.git &&
	git diff-tree master^ master >actual &&
	test_cmp ../expect actual)
'

test_done