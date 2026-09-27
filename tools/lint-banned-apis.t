#!/usr/bin/env perl
# Tests for lint-banned-apis.pl. Run with: perl tools/lint-banned-apis.t
use strict;
use warnings;
use File::Basename qw(dirname);
use File::Path qw(make_path);
use File::Spec;
use File::Temp qw(tempdir);
use Test::More;

my $script = File::Spec->catfile( dirname(__FILE__), 'lint-banned-apis.pl' );

# Writes each name => content pair under a fresh directory, lints it, and
# returns the exit code and the combined output.
sub lint
{
    my %files = @_;
    my $dir = tempdir( CLEANUP => 1 );
    for my $name ( keys %files )
    {
        my $path = File::Spec->catfile( $dir, $name );
        make_path( dirname($path) );
        open my $fh, '>', $path or die "$path: $!";
        print $fh $files{$name};
        close $fh;
    }
    my $output = `"$^X" "$script" "$dir" 2>&1`;
    return ( $? >> 8, $output );
}

sub flags
{
    my ( $code, $rule, $name ) = @_;
    my ( $exit, $output ) = lint( 'a.cpp' => $code );
    is( $exit, 1, "$name: fails" );
    like( $output, qr/a\.cpp:\d+: \[\Q$rule\E\]/, "$name: reports [$rule]" )
        or diag $output;
}

sub passes
{
    my ( $code, $name ) = @_;
    my ( $exit, $output ) = lint( 'a.cpp' => $code );
    is( $exit, 0, "$name: passes" ) or diag $output;
}

# --- A clean tree -----------------------------------------------------------
passes( "int f() { return 0; }\n", 'clean file' );

# --- keyword ----------------------------------------------------------------
flags( "void f() { throw 1; }\n",                  'keyword', 'throw' );
flags( "void f() { try { g(); } catch(...) {} }\n", 'keyword', 'try/catch' );
passes( "void* p = new (std::nothrow) int;\n",      'std::nothrow is not throw' );
passes( "// try again later\n",                     'keyword in a line comment' );
passes( "/* throw\n   catch */ int x;\n",           'keyword in a block comment' );
passes( "const char* s = \"try it\";\n",            'keyword in a string' );
passes( "const char* s = \"a \\\" try\";\n",        'keyword after an escaped quote' );
passes( "const char* s = R\"x(throw \")x\";\n",     'keyword in a raw string' );
passes( "int n = 1'000; bool try_ = true;\n",       'digit separator, identifier' );

# --- stox -------------------------------------------------------------------
flags( "int n = std::stoi( s );\n",       'stox', 'std::stoi' );
flags( "auto n = std::stoull(s);\n",      'stox', 'std::stoull' );
flags( "double d = stod( s );\n",         'stox', 'unqualified stod' );
passes( "int n = 0; std::from_chars( b, e, n );\n", 'std::from_chars' );
passes( "int stoiCount = 0;\n",           'identifier starting with stoi' );

# --- regex, random-device, thread -------------------------------------------
flags( "#include <regex>\n",                  'regex', '#include <regex>' );
flags( "std::regex r( \"a\" );\n",            'regex', 'std::regex' );
flags( "std::random_device rd;\n",            'random-device', 'std::random_device' );
flags( "std::thread t( f );\n",               'thread', 'std::thread' );
flags( "std::jthread t( f );\n",              'thread', 'std::jthread' );
passes( "std::this_thread::sleep_for( d );\n", 'std::this_thread' );
passes( "std::thread::id id;\n",               'std::thread::id' );

# --- at, expected-value -----------------------------------------------------
flags( "int x = v.at( 3 );\n",   'at', '.at(' );
flags( "int x = p->at(3);\n",    'at', '->at(' );
passes( "int x = v[3]; atExit();\n", 'subscript, identifier at' );
flags( "int x = e.value();\n",   'expected-value', '.value()' );
passes( "int x = *e; int y = e.value_or( 0 );\n", 'dereference, value_or' );

# --- string-position --------------------------------------------------------
flags( "auto t = s.substr( 1 );\n",       'string-position', 'substr' );
flags( "int c = s->compare( 0, 2, t );\n", 'string-position', 'compare' );
flags( "s.replace( 0, 1, t );\n",         'string-position', 'replace' );
flags( "s.copy( buf, 4 );\n",             'string-position', 'copy' );
flags( "s.erase( 0, 2 );\n",              'string-position', 'erase with a literal position' );
flags( "s.insert( 3, t );\n",             'string-position', 'insert with a literal position' );
passes( "v.erase( it ); m.erase( key ); v.insert( v.end(), x );\n", 'container erase/insert' );
passes( "std::copy( a, b, c );\n",        'std::copy' );

# --- filesystem -------------------------------------------------------------
flags( "bool b = std::filesystem::exists( p );\n", 'filesystem', 'throwing exists' );
flags( "std::filesystem::remove(\n    p );\n",     'filesystem', 'multi-line throwing call' );
passes( "std::error_code ec;\nbool b = std::filesystem::exists( p, ec );\n",
        'error_code overload' );
passes( "std::error_code err;\nstd::filesystem::copy(\n    a,\n    b, err );\n",
        'multi-line error_code overload' );
flags( "int ec;\nbool b = std::filesystem::exists( p, ec );\n", 'filesystem',
       'last argument is not a std::error_code' );
passes( "std::filesystem::path p( \"a\" );\n",     'filesystem::path is a type' );
flags( "namespace fs = std::filesystem;\n",        'filesystem', 'namespace alias' );
flags( "using namespace std::filesystem;\n",       'filesystem', 'using-directive' );

# --- suppressions -----------------------------------------------------------
passes( "int x = v.at( 3 ); // ADR-0004-allow(at): 3 < v.size() by construction\n",
        'suppression naming the rule' );
flags( "int x = v.at( 3 ); // ADR-0004-allow(stox): wrong rule\n", 'at',
       'suppression naming another rule' );
flags( "int x = v.at( 3 ); // ADR-0004-allow(at):\n", 'suppression',
       'suppression without a reason' );
flags( "int x = 0; // ADR-0004-allow(nonsense): why\n", 'suppression',
       'suppression naming an unknown rule' );
flags( "int x = 0; // ADR-0004-allow(at): nothing here\n", 'suppression',
       'suppression that suppresses nothing' );
passes( "std::filesystem::remove( // ADR-0004-allow(filesystem): fatal is fine here\n    p );\n",
        'suppression on the first line of a multi-line call' );

passes( "int x = v.at( 0 ) + e.value(); // ADR-0004-allow(at): ok ADR-0004-allow(expected-value): ok\n",
        'two suppressions on one line' );
flags( "const char* s = \"ADR-0004-allow(at): x\"; int x = v.at( 0 );\n", 'at',
       'a suppression inside a string literal does not count' );

# --- lexing edge cases ------------------------------------------------------
passes( "// spliced \\\nthrow\n", 'a line comment continued by a splice is still comment' );
passes( "const char* s = \"abc\\\ndef\";\nconst char* t = \" try \";\n",
        'a string continued by a splice keeps quotes paired' );
passes( "#error try again\n#warning catch this\n", '#error and #warning text' );
passes( "int n = foo::stoi( s );\n", 'a stoi in another namespace' );
flags( "int n = ::std::stoi( s );\n", 'stox', 'globally qualified std::stoi' );

# --- filesystem constructors ------------------------------------------------
flags( "std::filesystem::directory_iterator it( p );\n", 'filesystem',
       'directory_iterator constructor with a variable name' );
passes( "std::error_code ec;\nstd::filesystem::directory_iterator it( p, ec );\n",
        'directory_iterator constructor with an error_code' );

# --- usage ------------------------------------------------------------------
{
    my $output = `"$^X" "$script" 2>&1`;
    is( $? >> 8, 2, 'no arguments is a usage error' );
}

# --- file selection ---------------------------------------------------------
{
    my ( $exit, $output ) = lint(
        'Plugins/b.h' => "std::random_device rd;\n",
        'notes.txt'   => "std::stoi\n",
    );
    is( $exit, 1, 'headers in subdirectories are linted' );
    like( $output, qr/b\.h:1: \[random-device\]/, 'reports the header hit' );
    unlike( $output, qr/notes\.txt/, 'non-source files are skipped' );
}

# --- output -----------------------------------------------------------------
{
    my ( $exit, $output ) = lint( 'a.cpp' => "int a;\nint n = std::stoi( s );\n" );
    like( $output, qr/a\.cpp:2: \[stox\] .*ADR-0004/, 'hit names the line, rule and ADR' );
}

done_testing();
