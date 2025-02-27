--TEST--
Tideways: Fetch Fatal Error Backtrace
--SKIPIF--
<?php
if (PHP_VERSION_ID >= 70000) {
    die("skip: Dont need error callback overwrite on PHP 7+");
}
--FILE--
<?php

function foo() {
    bar();
}

function bar() {
    unknown();
}

register_shutdown_function(function () {
    var_dump(array_slice(tideways_fatal_backtrace(), 0, 2));
});

var_dump(tideways_fatal_backtrace()); // before enabled

tideways_enable();

var_dump(tideways_fatal_backtrace()); // before anything happend

foo();
--EXPECTF--
NULL
NULL

Fatal error: Call to undefined function unknown() in %s on line 8
array(2) {
  [0]=>
  array(4) {
    ["file"]=>
    string(%d) "%s"
    ["line"]=>
    int(4)
    ["function"]=>
    string(3) "bar"
    ["args"]=>
    array(0) {
    }
  }
  [1]=>
  array(4) {
    ["file"]=>
    string(%d) "%s"
    ["line"]=>
    int(21)
    ["function"]=>
    string(3) "foo"
    ["args"]=>
    array(0) {
    }
  }
}
