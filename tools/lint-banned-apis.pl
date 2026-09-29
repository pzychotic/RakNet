#!/usr/bin/env perl
# Lints C++ sources for the APIs that ADR-0004 bans from Source/.
#
#   perl tools/lint-banned-apis.pl Source
#
# The rules below mirror docs/adr/0004-raknet-is-exception-neutral.md: the
# `throw`/`try`/`catch` ban from "Decision" point 1, and every entry of "What is
# off-limits in `Source/`". Change the two together.
#
# This is a text lint, not a type-aware one. Comments, literals and #error text
# are blanked out first, then each rule matches the remaining code. It cannot
# tell a std::string from a std::vector, so it errs broad where Source/ has no
# legitimate use and narrow where it has many:
#   - `substr`, `compare`, `replace` and `copy` member calls are flagged in every
#     form and on every receiver. `erase`/`insert` are flagged only with a
#     literal position. The `std::string( s, pos, n )` substring constructor is
#     not flagged at all.
#   - `.value()` is flagged on every receiver, not only std::expected.
#   - `std::thread` is flagged wherever it is named, not only where constructed.
#   - the exception-mode macros are flagged wherever they are named, not only
#     where a branch tests them.
#   - a std::filesystem call or constructor passes when its last argument is a
#     name declared as a std::error_code anywhere in the same file. Members of
#     directory_entry and a directory iterator's `++` also throw, and are not
#     flagged; a braced constructor `directory_iterator{ p }` is not flagged.
# A hit that is safe gets a suppression on the line where the hit starts:
#
#   x = v.at( i ); // ADR-0004-allow(at): i < v.size(), checked above
#
# The rule name and a reason are both required. A suppression that suppresses
# nothing is itself an error, so none outlive the code they excused.
#
# Exit status: 0 clean, 1 hits, 2 usage error.
use strict;
use warnings;
use File::Find qw(find);

my $OFF_LIMITS = 'ADR-0004, "What is off-limits in `Source/`"';
my $SELF       = 'tools/lint-banned-apis.pl';

my @RULES = (
    {
        id      => 'keyword',
        re      => qr/\b(?:throw|try|catch)\b/,
        message => '`throw`, `try` and `catch` are banned. Report failure by return value',
        section => 'ADR-0004, "Decision" point 1',
    },
    {
        id      => 'stox',
        re      => qr/(?<![\w.>:])(?:::)?(?:std::)?sto(?:i|l|ll|ul|ull|f|d|ld)\s*\(/,
        message => 'std::stoi and relatives throw on bad input. Use std::from_chars',
    },
    {
        id      => 'filesystem',
        check   => \&check_filesystem,
        message => 'std::filesystem call without a std::error_code as its last argument. '
                 . 'Use the error_code overload',
    },
    {
        id      => 'random-device',
        re      => qr/\brandom_device\b/,
        message => 'std::random_device throws. Use RakNet::FillRandomBytes (ADR-0001)',
    },
    {
        id      => 'regex',
        re      => qr/\bstd::(?:w?regex|basic_regex|regex_\w+)\b|#\s*include\s*<regex>/,
        message => 'std::regex throws on a bad pattern and on excess complexity',
    },
    {
        id      => 'at',
        re      => qr/(?:\.|->)\s*at\s*\(/,
        message => '.at() throws std::out_of_range. Bound the index first and use []',
    },
    {
        id      => 'string-position',
        re      => qr/(?:\.|->)\s*(?:substr|compare|replace|copy)\s*\(|(?:\.|->)\s*(?:erase|insert)\s*\(\s*[0-9]/,
        message => 'std::string position-taking members throw std::out_of_range. '
                 . 'Bound the position first, or use an iterator or std::string_view form',
    },
    {
        id      => 'expected-value',
        re      => qr/(?:\.|->)\s*value\s*\(\s*\)/,
        message => 'std::expected::value() throws when empty. Check has_value() and use * or ->',
    },
    {
        id      => 'thread',
        re      => qr/\bstd::j?thread\b(?!\s*::\s*(?:id|hardware_concurrency)\b)/,
        message => 'The std::thread constructor throws. Create threads through RakThread',
    },
    {
        id      => 'exception-mode',
        re      => qr/\b(?:__cpp_exceptions|__EXCEPTIONS|__GXX_EXCEPTIONS|_CPPUNWIND|_HAS_EXCEPTIONS|cxx_exceptions)\b/,
        message => 'A branch on the exception mode runs in only one mode, and CI tests only one. '
                 . 'A suppression reopens exception-neutral issue 06',
    },
);
my %RULE_BY_ID = map { $_->{id} => $_ } @RULES;

my %SOURCE_EXTENSION = map { $_ => 1 } qw(c cc cpp cxx h hh hpp hxx inl ipp);

# std::filesystem names that are types or enums rather than calls with an
# error_code overload.
my %FILESYSTEM_NON_CALL = map { $_ => 1 } qw(
    path file_status space_info filesystem_error file_type perms perm_options
    copy_options directory_options file_time_type u8path
);

@ARGV or ( print STDERR "usage: $0 <directory>...\n" ), exit 2;
my @files;
for my $root (@ARGV)
{
    -d $root or ( print STDERR "$0: not a directory: $root\n" ), exit 2;
    find(
        {
            no_chdir => 1,
            wanted   => sub {
                push @files, $File::Find::name
                    if -f $_ && /\.(\w+)$/ && $SOURCE_EXTENSION{ lc $1 };
            },
        },
        $root
    );
}

my $problems = 0;
lint_file($_) for sort @files;
if ($problems)
{
    print "$problems banned-API hit(s). See docs/adr/0004-raknet-is-exception-neutral.md.\n";
    exit 1;
}
exit 0;

sub report
{
    my ( $file, $line, $rule, $message, $section ) = @_;
    ( my $shown = $file ) =~ s{\\}{/}g;
    print "$shown:$line: [$rule] $message ($section)\n";
    print "::error file=$shown,line=$line,title=ADR-0004 [$rule]::$message\n"
        if $ENV{GITHUB_ACTIONS};
    ++$problems;
}

sub lint_file
{
    my ($file) = @_;
    open my $fh, '<', $file or die "$file: $!";
    my $text = do { local $/; <$fh> };
    close $fh;
    $text =~ s/\r\n?/\n/g;

    my ( $code, $comments ) = split_code_and_comments($text);
    my $suppressions = parse_suppressions( $file, $comments );

    for my $rule (@RULES)
    {
        my @offsets = $rule->{check} ? $rule->{check}->($code) : match_offsets( $code, $rule->{re} );
        my %seen_line;
        for my $offset (@offsets)
        {
            my $line = 1 + ( substr( $code, 0, $offset ) =~ tr/\n// );
            next if $seen_line{$line}++;
            if ( exists $suppressions->{$line}{ $rule->{id} } )
            {
                $suppressions->{$line}{ $rule->{id} } = 1;
                next;
            }
            report( $file, $line, $rule->{id}, $rule->{message}, $rule->{section} // $OFF_LIMITS );
        }
    }

    for my $line ( sort { $a <=> $b } keys %$suppressions )
    {
        for my $id ( sort keys %{ $suppressions->{$line} } )
        {
            report( $file, $line, 'suppression', "ADR-0004-allow($id) suppresses nothing on this line", $SELF )
                unless $suppressions->{$line}{$id};
        }
    }
}

sub match_offsets
{
    my ( $code, $re ) = @_;
    my @offsets;
    push @offsets, $-[0] while $code =~ /$re/g;
    return @offsets;
}

# Returns { line => { rule => 0 } } for the well-formed suppressions in the
# comment layer and reports the malformed ones. A reason runs to the next
# suppression, the end of the comment, or the end of the line.
sub parse_suppressions
{
    my ( $file, $comments ) = @_;
    my %suppressions;
    my $line = 0;
    for ( split /\n/, $comments, -1 )
    {
        ++$line;
        while (/ADR-0004-allow\(([^)]*)\)(:?)((?:(?!ADR-0004-allow\(|\*\/).)*)/g)
        {
            my ( $id, $has_colon, $reason ) = ( $1, $2 ne '', $3 );
            $reason =~ s/^\s+|\s+$//g;
            if ( !$RULE_BY_ID{$id} )
            {
                report( $file, $line, 'suppression', "ADR-0004-allow($id) names no lint rule", $SELF );
            }
            elsif ( !$has_colon || $reason eq '' )
            {
                report( $file, $line, 'suppression', "ADR-0004-allow($id) needs a reason after the colon",
                        $SELF );
            }
            else
            {
                $suppressions{$line}{$id} = 0;
            }
        }
    }
    return \%suppressions;
}

# Splits the text into two layers of the same length: the code, with comments,
# literals and #error/#warning text blanked to spaces, and the comments, with
# everything else blanked. Every newline is kept in both, so offsets still map
# to the original lines. A backslash-newline splice continues a comment or a
# literal, as it does in the compiler.
sub split_code_and_comments
{
    my ($text) = @_;
    my ( $code, $comments ) = ( '', '' );
    my $blank = sub { ( my $s = shift ) =~ s/[^\n]/ /g; $s };
    my $keep_code    = sub { $code .= $_[0];         $comments .= $blank->( $_[0] ) };
    my $keep_comment = sub { $code .= $blank->( $_[0] ); $comments .= $_[0] };
    my $keep_neither = sub { $code .= $blank->( $_[0] ); $comments .= $blank->( $_[0] ) };
    pos($text) = 0;
    while ( pos($text) < length $text )
    {
        if    ( $text =~ /\G(\/\/(?:\\.|[^\n\\])*)/gcs )                               { $keep_comment->($1) }
        elsif ( $text =~ /\G(\/\*.*?(?:\*\/|\z))/gcs )                                 { $keep_comment->($1) }
        elsif ( $text =~ /\G((?:u8|[uUL])?R"([^()\\\s]{0,16})\(.*?\)\2")/gcs )        { $keep_neither->($1) }
        elsif ( $text =~ /\G((?:u8|[uUL])?"(?:\\.|[^"\\\n])*"?)/gcs )                  { $keep_neither->($1) }
        elsif ( $text =~ /\G((?:u8|[uUL])?'(?:\\.|[^'\\\n])*'?)/gcs )                  { $keep_neither->($1) }
        # The message of #error and #warning is free text, not code.
        elsif ( $text =~ /\G(?<![^\n])([ \t]*#[ \t]*(?:error|warning)\b)((?:\\.|[^\n\\])*)/gcs )
        {
            $keep_code->($1);
            $keep_neither->($2);
        }
        # A whole word at once, so a quote right after one is a digit separator.
        elsif ( $text =~ /\G(\w+'?)/gc )                                               { $keep_code->($1) }
        elsif ( $text =~ /\G(.)/gcs )                                                  { $keep_code->($1) }
    }
    return ( $code, $comments );
}

# Returns the offset of every std::filesystem call whose last argument is not a
# name declared as a std::error_code in the file, plus every alias or
# using-declaration that would hide calls from this check.
sub check_filesystem
{
    my ($code) = @_;
    my @offsets = match_offsets( $code,
        qr/\bnamespace\s+\w+\s*=\s*(?:::)?std::filesystem\b|\busing\s+(?:namespace\s+)?(?:::)?std::filesystem\b/ );

    # A call, or a constructor written with a variable name: `directory_iterator it( p )`.
    while ( $code =~ /\bfilesystem::(\w+)(?:\s+\w+)?\s*\(/g )
    {
        my ( $start, $name, $resume ) = ( $-[0], $1, pos($code) );
        next if $FILESYSTEM_NON_CALL{$name};

        my ( $depth, $arg, @args ) = ( 1, '' );
        pos($code) = $resume;
        while ( $depth && $code =~ /\G(.)/gcs )
        {
            my $c = $1;
            if    ( $c =~ /[(\[{]/ ) { ++$depth }
            elsif ( $c =~ /[)\]}]/ ) { --$depth; last unless $depth }
            if    ( $c eq ',' && $depth == 1 ) { push @args, $arg; $arg = '' }
            else                               { $arg .= $c }
        }
        push @args, $arg;
        pos($code) = $resume;

        my ($last) = $args[-1] =~ /^\s*(\w+)\s*$/;
        push @offsets, $start
            unless $last && $code =~ /\berror_code\s*[&*]?\s*\b\Q$last\E\b/;
    }
    return sort { $a <=> $b } @offsets;
}
