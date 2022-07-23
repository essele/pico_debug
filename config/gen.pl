#!/usr/bin/perl -w

use strict;
use Data::Dumper;

my %structs = ();


sub handle_new_item {
	my $itemname = shift @_;
	my @elems = split(/\./, $itemname);
	my $item = pop @elems;

	# Now make sure we have a structure setup for each item in the list
	# and keep a reference to the final one so we can add the item
	my $ref = \%structs;
	foreach my $x (@elems) {
		if (not exists ${$ref}{$x}) {
			${$ref}{$x} = { "x"=>1, "y"=> 2 };
			print("Created $x\n");
			$ref = %{$ref}{$x};
		}
	}
	#
	# Now put the actual item in the list...
	#
	${$ref}{$item} = { "item" => 1 };
	$ref = %{$ref}{$item};
	return $ref;
}



while (<>) {
	print($_);
	if (/^([\w\.]+):/) {
		print("GOT word: $1\n");
		my $ref = handle_new_item($1);
		${$ref}{"lee"} = 45;
		next;
	}
}

print Dumper(%structs);
