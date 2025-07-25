/*
 * Help text display for CCHD.
 * 
 * Provides two levels of help: concise for quick reminders and verbose for
 * full documentation. This dual approach balances discoverability with avoiding
 * information overload. Help text is kept in sync with actual functionality
 * to prevent documentation drift.
 */

#pragma once

// Display concise help when no arguments are provided.
// Shows only essential usage to avoid overwhelming new users while encouraging
// them to use --help for detailed information. This appears on stderr to keep
// stdout clean for pipeline usage.
void cchd_print_concise_help(const char *program_name);

// Display full help for --help flag with complete option documentation.
// Includes all options, environment variables, and examples. Goes to stdout
// as this is explicitly requested documentation, not an error condition.
void cchd_print_verbose_usage(const char *program_name);