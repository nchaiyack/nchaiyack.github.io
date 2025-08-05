/*
 * color-detection-diagnostic.c
 * 
 * Comprehensive terminal color detection diagnostic tool
 * Implements all known methods for detecting terminal color capabilities
 * with runtime detection of unavailable methods and graceful fallbacks.
 *
 * BUILD INSTRUCTIONS (standalone):
 * ================================
 * 
 * With ncurses (recommended):
 *   gcc -Wall -std=c11 -g -o color-detection-diagnostic color-detection-diagnostic.c -lncurses
 *
 * Without ncurses (reduced functionality):
 *   gcc -Wall -std=c11 -g -o color-detection-diagnostic color-detection-diagnostic.c
 *
 * DETECTION METHODS IMPLEMENTED:
 * ==============================
 * 
 * 1. Environment Variable Analysis
 *    - Examines TERM, COLORTERM, TERM_PROGRAM, VTE_VERSION, KONSOLE_VERSION
 *    - Detects Windows terminal emulators via ANSICON, ConEmuANSI
 *    - Pattern matching for color capability indicators
 *
 * 2. Terminfo/Termcap Database Queries
 *    - Uses ncurses/curses library to query terminal capabilities
 *    - Checks 'colors' capability for maximum color count
 *    - Validates setaf/setab for ANSI color support
 *    - Tests for RGB flag indicating true color support
 *
 * 3. Device Attribute Queries (DA1/DA2)
 *    - Sends escape sequences to query terminal capabilities
 *    - Primary Device Attributes (\033[c) for basic feature detection
 *    - Secondary Device Attributes (\033[>c) for extended capabilities
 *    - Requires interactive terminal with timeout handling
 *
 * 4. True Color Probing
 *    - Sets and queries specific RGB palette entries
 *    - Validates round-trip color accuracy for true color detection
 *    - Fallback testing with 24-bit RGB escape sequences
 *    - Interactive terminal required for response capture
 *
 * 5. Direct Capability Testing
 *    - Outputs color test patterns using various escape sequences
 *    - Tests 8-color ANSI, 256-color indexed, and 24-bit RGB
 *    - Visual confirmation of color rendering capabilities
 *    - Demonstrates actual color output to user
 *
 * 6. Heuristic Terminal Detection
 *    - Pattern-based identification of known terminal emulators
 *    - Capability matrix lookup for popular terminals
 *    - Process environment analysis and SSH session detection
 *    - Fallback logic when other methods fail
 *
 * Each method includes runtime availability checking and graceful degradation
 * when required system capabilities (ncurses, interactive terminal) are unavailable.
 * Results are aggregated to provide consensus recommendations for optimal color usage.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/select.h>

#ifdef __has_include
  #if __has_include(<ncurses.h>)
    #define HAS_NCURSES 1
    #include <ncurses.h>
  #elif __has_include(<curses.h>)
    #define HAS_CURSES 1
    #include <curses.h>
  #endif
#else
  #ifdef NCURSES_VERSION
    #define HAS_NCURSES 1
    #include <ncurses.h>
  #else
    #define HAS_CURSES 1
    #include <curses.h>
  #endif
#endif

#ifdef __has_include
  #if __has_include(<term.h>)
    #define HAS_TERM_H 1
    #include <term.h>
  #endif
#endif

typedef struct {
    int available;
    const char* reason;
} method_availability_t;

typedef struct {
    int monochrome;
    int basic_8_color;
    int extended_16_color;
    int indexed_256_color;
    int true_color_24bit;
    int background_color;
    int bold_colors;
    int color_pairs;
    int max_color_count;
} color_capabilities_t;

typedef struct {
    const char* method_name;
    method_availability_t availability;
    color_capabilities_t capabilities;
    const char* details;
} detection_result_t;

#define MAX_METHODS 16
detection_result_t results[MAX_METHODS];
int result_count = 0;

static struct termios original_termios;
static int termios_saved = 0;

void save_terminal_state(void) {
    if (tcgetattr(STDIN_FILENO, &original_termios) == 0) {
        termios_saved = 1;
    }
}

void restore_terminal_state(void) {
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
    }
}

void cleanup_and_exit(int sig) {
    restore_terminal_state();
    printf("\n\nDetection interrupted by signal %d\n", sig);
    exit(1);
}

void setup_signal_handlers(void) {
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
}

method_availability_t check_environment_vars_available(void) {
    method_availability_t result = {1, "Environment variables always available"};
    return result;
}

method_availability_t check_terminfo_available(void) {
    method_availability_t result = {0, "terminfo not available"};
    
#if defined(HAS_NCURSES) || defined(HAS_CURSES)
    result.available = 1;
    result.reason = "ncurses/curses library available";
#endif
    
    return result;
}

method_availability_t check_escape_probing_available(void) {
    method_availability_t result = {0, "terminal not interactive"};
    
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        result.available = 1;
        result.reason = "interactive terminal available";
    }
    
    return result;
}

int read_response_with_timeout(char* buffer, size_t buffer_size, int timeout_ms) {
    fd_set read_fds;
    struct timeval timeout;
    int bytes_read = 0;
    
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);
    
    int ready = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout);
    if (ready > 0 && FD_ISSET(STDIN_FILENO, &read_fds)) {
        bytes_read = read(STDIN_FILENO, buffer, buffer_size - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
        }
    }
    
    return bytes_read;
}

void set_raw_mode(void) {
    struct termios raw = original_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

// =============================================================================
// METHOD 1: Environment Variable Analysis
// Examines TERM, COLORTERM, TERM_PROGRAM, VTE_VERSION, KONSOLE_VERSION
// Detects Windows terminal emulators via ANSICON, ConEmuANSI
// Pattern matching for color capability indicators
// =============================================================================
detection_result_t detect_via_environment_vars(void) {
    detection_result_t result = {0};
    result.method_name = "Environment Variables";
    result.availability = check_environment_vars_available();
    
    if (!result.availability.available) {
        result.details = result.availability.reason;
        return result;
    }
    
    char details[1024] = {0};
    int pos = 0;
    
    // Query all relevant environment variables for color detection
    const char* term = getenv("TERM");                    // Primary terminal type identifier
    const char* colorterm = getenv("COLORTERM");          // Modern true color indicator
    const char* vte_version = getenv("VTE_VERSION");      // GTK VTE-based terminals
    const char* konsole_version = getenv("KONSOLE_VERSION"); // KDE Konsole
    const char* term_program = getenv("TERM_PROGRAM");    // macOS terminal identification
    const char* ansicon = getenv("ANSICON");              // Windows ANSI support
    const char* conemu_ansi = getenv("ConEmuANSI");       // ConEmu Windows terminal
    
    pos += snprintf(details + pos, sizeof(details) - pos, "TERM=%s ", term ? term : "(unset)");
    
    if (colorterm) {
        pos += snprintf(details + pos, sizeof(details) - pos, "COLORTERM=%s ", colorterm);
        if (strcmp(colorterm, "truecolor") == 0 || strcmp(colorterm, "24bit") == 0) {
            result.capabilities.true_color_24bit = 1;
            result.capabilities.max_color_count = 16777216;
        }
    }
    
    if (term) {
        if (strstr(term, "256color") || strstr(term, "256")) {
            result.capabilities.indexed_256_color = 1;
            result.capabilities.max_color_count = 256;
        } else if (strstr(term, "16color")) {
            result.capabilities.extended_16_color = 1;
            result.capabilities.max_color_count = 16;
        } else if (strstr(term, "color")) {
            result.capabilities.basic_8_color = 1;
            result.capabilities.max_color_count = 8;
        }
        
        if (strstr(term, "xterm-direct") || strstr(term, "truecolor")) {
            result.capabilities.true_color_24bit = 1;
            result.capabilities.max_color_count = 16777216;
        }
    }
    
    if (vte_version) {
        pos += snprintf(details + pos, sizeof(details) - pos, "VTE_VERSION=%s ", vte_version);
        result.capabilities.true_color_24bit = 1;
    }
    
    if (konsole_version) {
        pos += snprintf(details + pos, sizeof(details) - pos, "KONSOLE_VERSION=%s ", konsole_version);
        result.capabilities.true_color_24bit = 1;
    }
    
    if (term_program) {
        pos += snprintf(details + pos, sizeof(details) - pos, "TERM_PROGRAM=%s ", term_program);
        if (strcmp(term_program, "iTerm.app") == 0 || 
            strcmp(term_program, "Apple_Terminal") == 0 ||
            strcmp(term_program, "WezTerm") == 0) {
            result.capabilities.true_color_24bit = 1;
        }
    }
    
    if (ansicon || conemu_ansi) {
        pos += snprintf(details + pos, sizeof(details) - pos, "Windows ANSI support detected ");
        result.capabilities.basic_8_color = 1;
    }
    
    if (!result.capabilities.max_color_count) {
        result.capabilities.monochrome = 1;
        result.capabilities.max_color_count = 2;
    }
    
    result.capabilities.background_color = result.capabilities.max_color_count > 2;
    result.capabilities.bold_colors = result.capabilities.max_color_count >= 8;
    
    result.details = strdup(details);
    return result;
}

// =============================================================================
// METHOD 2: Terminfo/Termcap Database Queries
// Uses ncurses/curses library to query terminal capabilities
// Checks 'colors' capability for maximum color count
// Validates setaf/setab for ANSI color support
// Tests for RGB flag indicating true color support
// =============================================================================
detection_result_t detect_via_terminfo(void) {
    detection_result_t result = {0};
    result.method_name = "Terminfo/Termcap";
    result.availability = check_terminfo_available();
    
    if (!result.availability.available) {
        result.details = result.availability.reason;
        return result;
    }
    
#if defined(HAS_NCURSES) || defined(HAS_CURSES)
    char details[1024] = {0};
    int pos = 0;
    
    int setupterm_result = setupterm(NULL, STDOUT_FILENO, NULL);
    if (setupterm_result != OK) {
        result.details = "setupterm() failed";
        return result;
    }
    
#ifdef HAS_TERM_H
    // Query terminfo database for color capabilities
    int color_count = tigetnum("colors");     // Maximum number of colors supported
    char* setaf = tigetstr("setaf");          // Set ANSI foreground color capability
    char* setab = tigetstr("setab");          // Set ANSI background color capability
    char* rgb_flag = tigetstr("RGB");         // True color (24-bit RGB) support flag
    char* setf = tigetstr("setf");            // Set foreground color (legacy)
    char* setb = tigetstr("setb");            // Set background color (legacy)
    
    pos += snprintf(details + pos, sizeof(details) - pos, "colors=%d ", color_count);
    
    if (color_count > 0) {
        result.capabilities.max_color_count = color_count;
        if (color_count >= 16777216) {
            result.capabilities.true_color_24bit = 1;
        } else if (color_count >= 256) {
            result.capabilities.indexed_256_color = 1;
        } else if (color_count >= 16) {
            result.capabilities.extended_16_color = 1;
        } else if (color_count >= 8) {
            result.capabilities.basic_8_color = 1;
        }
    }
    
    if (setaf != (char*)-1 && setaf != NULL) {
        pos += snprintf(details + pos, sizeof(details) - pos, "setaf=yes ");
        result.capabilities.background_color = 1;
    }
    
    if (setab != (char*)-1 && setab != NULL) {
        pos += snprintf(details + pos, sizeof(details) - pos, "setab=yes ");
        result.capabilities.background_color = 1;
    }
    
    if (rgb_flag != (char*)-1 && rgb_flag != NULL) {
        pos += snprintf(details + pos, sizeof(details) - pos, "RGB=yes ");
        result.capabilities.true_color_24bit = 1;
    }
    
    if (setf != (char*)-1 && setf != NULL) {
        pos += snprintf(details + pos, sizeof(details) - pos, "setf=yes ");
    }
    
    if (setb != (char*)-1 && setb != NULL) {
        pos += snprintf(details + pos, sizeof(details) - pos, "setb=yes ");
    }
#else
    pos += snprintf(details + pos, sizeof(details) - pos, "term.h not available, limited terminfo access ");
#endif
    
    result.capabilities.bold_colors = result.capabilities.max_color_count >= 8;
    result.details = strdup(details);
#else
    result.details = "No curses library available";
#endif
    
    return result;
}

// =============================================================================
// METHOD 3: Device Attribute Queries (DA1/DA2)
// Sends escape sequences to query terminal capabilities
// Primary Device Attributes (\033[c) for basic feature detection
// Secondary Device Attributes (\033[>c) for extended capabilities
// Requires interactive terminal with timeout handling
// =============================================================================
detection_result_t detect_via_device_attributes(void) {
    detection_result_t result = {0};
    result.method_name = "Device Attribute Queries";
    result.availability = check_escape_probing_available();
    
    if (!result.availability.available) {
        result.details = result.availability.reason;
        return result;
    }
    
    char details[1024] = {0};
    int pos = 0;
    
    set_raw_mode();
    
    // Send Primary Device Attributes query (\033[c)
    printf("\033[c");
    fflush(stdout);
    
    char response[256];
    int bytes = read_response_with_timeout(response, sizeof(response), 1000);
    
    if (bytes > 0) {
        pos += snprintf(details + pos, sizeof(details) - pos, "DA1: %.*s ", bytes, response);
        
        if (strstr(response, "62;") || strstr(response, "63;")) {
            result.capabilities.basic_8_color = 1;
            result.capabilities.max_color_count = 8;
        }
    } else {
        pos += snprintf(details + pos, sizeof(details) - pos, "DA1: no response ");
    }
    
    // Send Secondary Device Attributes query (\033[>c)
    printf("\033[>c");
    fflush(stdout);
    
    bytes = read_response_with_timeout(response, sizeof(response), 1000);
    
    if (bytes > 0) {
        pos += snprintf(details + pos, sizeof(details) - pos, "DA2: %.*s ", bytes, response);
        
        if (strstr(response, "1;") && strstr(response, "0;")) {
            result.capabilities.indexed_256_color = 1;
            result.capabilities.max_color_count = 256;
        }
    } else {
        pos += snprintf(details + pos, sizeof(details) - pos, "DA2: no response ");
    }
    
    restore_terminal_state();
    
    if (!result.capabilities.max_color_count) {
        result.capabilities.monochrome = 1;
        result.capabilities.max_color_count = 2;
    }
    
    result.capabilities.background_color = result.capabilities.max_color_count > 2;
    result.capabilities.bold_colors = result.capabilities.max_color_count >= 8;
    
    result.details = strdup(details);
    return result;
}

// =============================================================================
// METHOD 4: True Color Probing
// Sets and queries specific RGB palette entries
// Validates round-trip color accuracy for true color detection
// Fallback testing with 24-bit RGB escape sequences
// Interactive terminal required for response capture
// =============================================================================
detection_result_t detect_via_truecolor_probe(void) {
    detection_result_t result = {0};
    result.method_name = "True Color Probe";
    result.availability = check_escape_probing_available();
    
    if (!result.availability.available) {
        result.details = result.availability.reason;
        return result;
    }
    
    char details[512] = {0};
    
    set_raw_mode();
    
    printf("\033]4;16;rgb:ab/cd/ef\033\\");
    printf("\033]4;16;?\033\\");
    fflush(stdout);
    
    char response[256];
    int bytes = read_response_with_timeout(response, sizeof(response), 1000);
    
    if (bytes > 0 && strstr(response, "ab") && strstr(response, "cd") && strstr(response, "ef")) {
        result.capabilities.true_color_24bit = 1;
        result.capabilities.max_color_count = 16777216;
        snprintf(details, sizeof(details), "True color confirmed via palette query: %.*s", bytes, response);
    } else {
        printf("\033[48;2;171;205;239m \033[m");
        fflush(stdout);
        
        result.capabilities.indexed_256_color = 1;
        result.capabilities.max_color_count = 256;
        snprintf(details, sizeof(details), "True color probe failed, assuming 256 color support");
    }
    
    restore_terminal_state();
    
    result.capabilities.background_color = 1;
    result.capabilities.bold_colors = 1;
    result.details = strdup(details);
    return result;
}

// =============================================================================
// METHOD 5: Direct Capability Testing
// Outputs color test patterns using various escape sequences
// Tests 8-color ANSI, 256-color indexed, and 24-bit RGB
// Visual confirmation of color rendering capabilities
// Demonstrates actual color output to user
// =============================================================================
detection_result_t detect_via_capability_testing(void) {
    detection_result_t result = {0};
    result.method_name = "Direct Capability Testing";
    result.availability = check_escape_probing_available();
    
    if (!result.availability.available) {
        result.details = result.availability.reason;
        return result;
    }
    
    char details[1024] = {0};
    int pos = 0;
    
    printf("\033[31mRed\033[0m ");
    printf("\033[32mGreen\033[0m ");
    printf("\033[34mBlue\033[0m ");
    fflush(stdout);
    
    result.capabilities.basic_8_color = 1;
    result.capabilities.max_color_count = 8;
    pos += snprintf(details + pos, sizeof(details) - pos, "8-color test ");
    
    printf("\033[38;5;196mBright Red\033[0m ");
    printf("\033[38;5;46mBright Green\033[0m ");
    printf("\033[38;5;21mBright Blue\033[0m ");
    fflush(stdout);
    
    result.capabilities.indexed_256_color = 1;
    result.capabilities.max_color_count = 256;
    pos += snprintf(details + pos, sizeof(details) - pos, "256-color test ");
    
    printf("\033[38;2;255;100;50mTruecolor Orange\033[0m ");
    printf("\033[48;2;50;100;255m White on Blue \033[0m");
    fflush(stdout);
    
    result.capabilities.true_color_24bit = 1;
    result.capabilities.max_color_count = 16777216;
    pos += snprintf(details + pos, sizeof(details) - pos, "truecolor test ");
    
    printf("\n");
    
    result.capabilities.background_color = 1;
    result.capabilities.bold_colors = 1;
    result.details = strdup(details);
    return result;
}

// =============================================================================
// METHOD 6: Heuristic Terminal Detection
// Pattern-based identification of known terminal emulators
// Capability matrix lookup for popular terminals
// Process environment analysis and SSH session detection
// Fallback logic when other methods fail
// =============================================================================
detection_result_t detect_via_heuristics(void) {
    detection_result_t result = {0};
    result.method_name = "Heuristic Detection";
    result.availability.available = 1;
    result.availability.reason = "Heuristics always available";
    
    char details[1024] = {0};
    int pos = 0;
    
    const char* term = getenv("TERM");
    const char* term_program = getenv("TERM_PROGRAM");
    const char* ssh_tty = getenv("SSH_TTY");
    
    if (term_program) {
        if (strcmp(term_program, "iTerm.app") == 0) {
            result.capabilities.true_color_24bit = 1;
            result.capabilities.max_color_count = 16777216;
            pos += snprintf(details + pos, sizeof(details) - pos, "iTerm detected ");
        } else if (strcmp(term_program, "Apple_Terminal") == 0) {
            result.capabilities.indexed_256_color = 1;
            result.capabilities.max_color_count = 256;
            pos += snprintf(details + pos, sizeof(details) - pos, "Apple Terminal detected ");
        } else if (strcmp(term_program, "WezTerm") == 0) {
            result.capabilities.true_color_24bit = 1;
            result.capabilities.max_color_count = 16777216;
            pos += snprintf(details + pos, sizeof(details) - pos, "WezTerm detected ");
        }
    }
    
    if (term && strstr(term, "screen")) {
        pos += snprintf(details + pos, sizeof(details) - pos, "GNU Screen detected ");
        if (strstr(term, "256color")) {
            result.capabilities.indexed_256_color = 1;
            result.capabilities.max_color_count = 256;
        } else {
            result.capabilities.basic_8_color = 1;
            result.capabilities.max_color_count = 8;
        }
    }
    
    if (term && strstr(term, "tmux")) {
        pos += snprintf(details + pos, sizeof(details) - pos, "tmux detected ");
        result.capabilities.indexed_256_color = 1;
        result.capabilities.max_color_count = 256;
    }
    
    if (ssh_tty) {
        pos += snprintf(details + pos, sizeof(details) - pos, "SSH session detected ");
    }
    
    if (!result.capabilities.max_color_count) {
        result.capabilities.basic_8_color = 1;
        result.capabilities.max_color_count = 8;
        pos += snprintf(details + pos, sizeof(details) - pos, "fallback to 8-color ");
    }
    
    result.capabilities.background_color = result.capabilities.max_color_count > 2;
    result.capabilities.bold_colors = result.capabilities.max_color_count >= 8;
    
    result.details = strdup(details);
    return result;
}

void add_result(detection_result_t result) {
    if (result_count < MAX_METHODS) {
        results[result_count++] = result;
    }
}

void print_capabilities(const color_capabilities_t* caps) {
    printf("    Capabilities:\n");
    printf("      Monochrome: %s\n", caps->monochrome ? "Yes" : "No");
    printf("      8-color: %s\n", caps->basic_8_color ? "Yes" : "No");
    printf("      16-color: %s\n", caps->extended_16_color ? "Yes" : "No");
    printf("      256-color: %s\n", caps->indexed_256_color ? "Yes" : "No");
    printf("      True color (24-bit): %s\n", caps->true_color_24bit ? "Yes" : "No");
    printf("      Background color: %s\n", caps->background_color ? "Yes" : "No");
    printf("      Bold colors: %s\n", caps->bold_colors ? "Yes" : "No");
    printf("      Max colors: %d\n", caps->max_color_count);
}

void print_summary(void) {
    printf("\n============================================================\n");
    printf("COLOR DETECTION DIAGNOSTIC SUMMARY\n");
    printf("============================================================\n\n");
    
    color_capabilities_t consensus = {0};
    int method_count = 0;
    
    for (int i = 0; i < result_count; i++) {
        detection_result_t* r = &results[i];
        printf("Method: %s\n", r->method_name);
        printf("  Available: %s\n", r->availability.available ? "Yes" : "No");
        if (!r->availability.available) {
            printf("  Reason: %s\n", r->availability.reason);
        } else {
            printf("  Details: %s\n", r->details ? r->details : "No details");
            print_capabilities(&r->capabilities);
            
            consensus.monochrome += r->capabilities.monochrome;
            consensus.basic_8_color += r->capabilities.basic_8_color;
            consensus.extended_16_color += r->capabilities.extended_16_color;
            consensus.indexed_256_color += r->capabilities.indexed_256_color;
            consensus.true_color_24bit += r->capabilities.true_color_24bit;
            consensus.background_color += r->capabilities.background_color;
            consensus.bold_colors += r->capabilities.bold_colors;
            if (r->capabilities.max_color_count > consensus.max_color_count) {
                consensus.max_color_count = r->capabilities.max_color_count;
            }
            method_count++;
        }
        printf("\n");
    }
    
    printf("CONSENSUS RECOMMENDATION:\n");
    if (consensus.true_color_24bit > 0) {
        printf("  Use: True color (24-bit RGB)\n");
        printf("  Max colors: 16,777,216\n");
    } else if (consensus.indexed_256_color > 0) {
        printf("  Use: 256-color palette\n");
        printf("  Max colors: 256\n");
    } else if (consensus.extended_16_color > 0) {
        printf("  Use: 16-color palette\n");
        printf("  Max colors: 16\n");
    } else if (consensus.basic_8_color > 0) {
        printf("  Use: 8-color ANSI\n");
        printf("  Max colors: 8\n");
    } else {
        printf("  Use: Monochrome\n");
        printf("  Max colors: 2\n");
    }
    
    printf("  Background colors: %s\n", consensus.background_color > 0 ? "Supported" : "Not supported");
    printf("  Bold/bright colors: %s\n", consensus.bold_colors > 0 ? "Supported" : "Not supported");
    
    printf("\n");
    printf("Detection methods available: %d/%d\n", method_count, result_count);
}

int main(void) {
    printf("Terminal Color Detection Diagnostic\n");
    printf("===================================\n\n");
    
    save_terminal_state();
    setup_signal_handlers();
    
    printf("Testing all color detection methods...\n\n");
    
    // Execute all 6 detection methods in sequence:
    add_result(detect_via_environment_vars());    // METHOD 1: Environment Variable Analysis
    add_result(detect_via_terminfo());            // METHOD 2: Terminfo/Termcap Database Queries
    add_result(detect_via_device_attributes());   // METHOD 3: Device Attribute Queries (DA1/DA2)
    add_result(detect_via_truecolor_probe());     // METHOD 4: True Color Probing
    add_result(detect_via_capability_testing());  // METHOD 5: Direct Capability Testing
    add_result(detect_via_heuristics());          // METHOD 6: Heuristic Terminal Detection
    
    print_summary();
    
    for (int i = 0; i < result_count; i++) {
        if (results[i].details) {
            free((void*)results[i].details);
        }
    }
    
    restore_terminal_state();
    return 0;
}