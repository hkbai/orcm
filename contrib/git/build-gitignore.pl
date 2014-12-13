#!/usr/bin/env perl
#
# Copyright (c) 2008-2014 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2014      Intel, Inc. All rights reserved.
# $COPYRIGHT$
#
# Dumb script to run through all the svn:ignore's in the tree and
# build build .gitignore files for Git.  

use strict;

use Getopt::Long;
use File::Copy;

my $verbose_arg = 0;
# Default to writing .gitignore
my $output_arg = ".gitignore";
my $help_arg = 0;

&Getopt::Long::Configure("bundling");
my $ok = Getopt::Long::GetOptions("verbose|v!" => \$verbose_arg,
                                  "output|o=s" => \$output_arg,
                                  "help|h!" => \$help_arg);

if (!$ok || $help_arg) {
    print "
Usage: $0 [-v] [-o output] [-h]\n";
    exit($ok);
}

print "Writing to: $output_arg\n"
    if ($verbose_arg);

open(OUT, ">$output_arg");

my @globals;

#############################################################################

print "Thinking...\n"
    if (!$verbose_arg);

# if we are not in an svn repo, then just concatenate
# the .gitignore_global and any .gitignore_local files
# to make the new .gitignore
if (! -d "./.svn") {
    print "Not in an svn repo - creating .gitignore from existing files\n"
      if ($verbose_arg);
    my @files = qw(.gitignore_global .gitignore_local);
    my @git;

    while (@files) {
        local $_ = shift @files;
        if (-f $_) {
            open(IN, $_) || die "Can't open $_";
            print "Reading $_...\n"
                if ($verbose_arg);
            while (<IN>) {
                chomp;
                push(@git, $_);
            }
            close(IN);
        }
    }
    
    foreach my $val (@git) {
        print OUT "$val\n";
    }
} else {
    # Put in some specials that we ignore everywhere
    @globals = qw/.libs
                  .deps
                  .libs
                  .svn
                  *.vpj
                  *.vpw
                  *.vpwhistu
                  *.vtg
                  *.swp
                  *.la
                  *.lo
                  *.o
                  *.so
                  *.a
                  .dirstamp
                  *.dSYM
                  *.S
                  *.loT
                  *.orig
                  *.rej
                  *.class
                  *.xcscheme
                  *.plist
                  *~
                  Makefile
                  Makefile.in
                  static-components.h
                  *\\\#/;
    unshift(@globals, "# Automatically generated by build-gitignore.pl; edits may be lost!");
    

    # add the globals */
    foreach my $val (@globals) {
        print OUT "$val\n";
    }

    # Start at the top level
    process(".");
}

close(OUT);

print "Wrote to $output_arg\n"
    if ($verbose_arg);

# Done!
exit(0);

#######################################################################

# DFS-oriented recursive directory search
sub process {
    my $dir = shift;
    my $outdir = $dir;
    $outdir =~ s/^\.\///;

    # Look at the svn:ignore property for this directory
    my $svn_ignore = `svn pg svn:ignore $dir 2> /dev/null`;
    # If svn failed, bail on this directory.
    return
        if ($? != 0);

    chomp($svn_ignore);
    if ($svn_ignore ne "") {
        print "Found svn:ignore in $dir\n"
            if ($verbose_arg);

        my @git;

        # See if there's an .gitignore_local file.  If so, add its
        # contents to the end.
        if (-f "$dir/.gitignore_local") {
            print "Reading $dir/.gitignore_local...\n"
                if ($verbose_arg);
            open(IN, "$dir/.gitignore_local") || die "Can't open .gitignore_local";
            while (<IN>) {
                chomp;
                push(@git, $_);
            }
            
            close(IN);
        }

        # Now read the svn:ignore value
        foreach my $line (split(/\n/, $svn_ignore)) {
            chomp($line);
            $line =~ s/^\.\///;
            next
                if ($line eq "");

            # Ensure not to ignore special git files
            next
                if ($line eq ".gitignore" || $line eq ".gitignore_local" ||
                    $line eq ".git" || $line eq ".svn");
            # We're globally ignoring some specials already; we can
            # skip those
            my $skip = 0;
            foreach my $g (@globals) {
                if ($g eq $line) {
                    $skip = 1;
                    last;
                }
            }
            next 
                if ($skip);

            push(@git, "$line");
        }

        # Write out a new .gitignore file
        foreach my $val (@git) {
            if ($outdir eq ".") {
                print OUT "$val\n";
            } else {
                print OUT "$outdir/$val\n";
            }
        }

    }
        
    # Now find subdirectories in this directory
    my @entries;
    opendir(DIR, $dir) || die "Cannot open directory \"$dir\" for reading: $!";
    @entries = sort(readdir(DIR));
    closedir DIR;

    foreach my $e (@entries) {
        # Skip special directories and sym links
        next
            if ($e eq "." || $e eq ".." || $e eq ".svn" || $e eq ".git" ||
                -l "$dir/$e");

        # If it's a directory, analyze it
        process("$dir/$e")
            if (-d "$dir/$e");
    }
}
