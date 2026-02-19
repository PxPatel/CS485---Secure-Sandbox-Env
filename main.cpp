#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/stat.h>
#include <fstream>
#include <sys/mount.h>


void print_usage(const char* program_name) {
    std::cerr << "Usage:\n"
              << "  " << program_name << " create\n"
              << "  " << program_name << " launch <program> [args...]\n"
              << "  " << program_name << " terminate <pid>\n";
}

// Helper to check if directory exists
bool dir_exists(const char* path) {
    struct stat info;
    return (stat(path, &info) == 0 && S_ISDIR(info.st_mode));
}

// Helper to create directory
bool create_dir(const char* path) {
    return mkdir(path, 0755) == 0;
}

// Helper to copy file
bool copy_file(const char* src, const char* dst) {
    std::ifstream source(src, std::ios::binary);
    std::ofstream dest(dst, std::ios::binary);
    
    if (!source || !dest) return false;
    
    dest << source.rdbuf();
    return true;
}

// Add this helper function before cmd_launch
int setup_chroot() {
    // Change to rootfs directory
    if (chdir("./rootfs") != 0) {
        perror("chdir to rootfs");
        return -1;
    }
    
    // Change root to current directory
    if (chroot(".") != 0) {
        perror("chroot");
        return -1;
    }
    
    // Change to new root
    if (chdir("/") != 0) {
        perror("chdir to /");
        return -1;
    }
    
    std::cout << "Successfully chrooted to rootfs\n";
    std::cout << "-----------------------------------------" << std::endl; //Empty line
    return 0;
}


int cmd_create() {
    bool needs_creation = false;

    if (dir_exists("./rootfs")) {
        std::cout << "rootfs already exists. Validating...\n";
        
        // Check essential directories
        const char* required_dirs[] = {"./rootfs/bin", "./rootfs/lib", "./rootfs/proc"};
        bool all_valid = true;
        
        for (const char* dir : required_dirs) {
            if (!dir_exists(dir)) {
                std::cerr << "Warning: " << dir << " not found\n";
                all_valid = false;
            }
        }
        
        if (all_valid) {
            std::cout << "Sandbox environment validated\n";
            return 0;
        }
        else {
            std::cout << "Some directories missing. Recreate? (y/n): ";
            char response;
            std::cin >> response;
            if (response != 'y' && response != 'Y') {
                return 1;
            }
            needs_creation = true;
        }
    } 
    else {
        std::cout << "rootfs not found. Create it automatically? (y/n): ";
        char response;
        std::cin >> response;
        
        if (response != 'y' && response != 'Y') {
            std::cout << "Aborted. Please create rootfs manually.\n";
            return 1;
        }
        needs_creation = true;
    }

    if (!needs_creation) {
        return 0; // Already validated
    }
        
    std::cout << "This process may take a while. Please be patient and do not interrupt the process...\n";

    std::cout << "Creating rootfs structure...\n";
    
    // Create directory structure
    const char* dirs[] = {
        "./rootfs",
        "./rootfs/bin",
        "./rootfs/lib",
        "./rootfs/lib64",
        "./rootfs/proc",
        "./rootfs/sys",
        "./rootfs/dev",
        "./rootfs/etc",
        "./rootfs/tmp"
    };
    
    for (const char* dir : dirs) {
        if (!create_dir(dir) && errno != EEXIST) {
            perror(dir);
            return 1;
        }
    }
    
    std::cout << "Copying essential binaries...\n";
    
    // Use system() to leverage bash for complex operations
    // Copy bash and its dependencies
    int ret = system("cp /bin/bash ./rootfs/bin/ 2>/dev/null");
    if (ret != 0) {
        std::cerr << "Warning: Failed to copy /bin/bash\n";
    }
    
    // Copy basic utilities
    system("cp /bin/ls ./rootfs/bin/ 2>/dev/null");
    system("cp /bin/cat ./rootfs/bin/ 2>/dev/null");
    system("cp /bin/sh ./rootfs/bin/ 2>/dev/null");
    
    std::cout << "Copying shared libraries...\n";
    
    // Copy common libraries (this is a simplified approach)
    system("cp /lib64/ld-linux-x86-64.so.2 ./rootfs/lib64/ 2>/dev/null");
    system("mkdir -p ./rootfs/lib/x86_64-linux-gnu");
    system("cp /lib/x86_64-linux-gnu/*.so* ./rootfs/lib/x86_64-linux-gnu/ 2>/dev/null");
    
    std::cout << "rootfs created successfully!\n";
    std::cout << "Note: This is a basic setup. Add more binaries as needed.\n";
    
    return 0;
}

int cmd_launch(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: No program specified\n";
        std::cerr << "Usage: launch <program> [args...]\n";
        return 1;
    }

    const char* program = argv[2];
    
    // Check if program exists on host
    if (access(program, F_OK) != 0) {
        std::cerr << "Error: Program not found: " << program << std::endl;
        return 1;
    }
    
    // Determine the target path in rootfs
    std::string target_path = "./rootfs";
    std::string program_name;
    
    // Extract just the filename from the path
    const char* last_slash = strrchr(program, '/');
    if (last_slash) {
        program_name = last_slash + 1;
    } else {
        program_name = program;
    }
    
    target_path += "/tmp/" + program_name;
    
    // Copy program into rootfs/tmp
    std::string copy_cmd = "cp ";
    copy_cmd += program;
    copy_cmd += " ";
    copy_cmd += target_path;
    
    if (system(copy_cmd.c_str()) != 0) {
        std::cerr << "Error: Failed to copy program to rootfs\n";
        return 1;
    }
    
    // Make it executable
    std::string chmod_cmd = "chmod +x " + target_path;
    system(chmod_cmd.c_str());
    
    std::cout << "Copied " << program << " to container\n";
    
    // The path inside the container will be /tmp/<filename>
    std::string container_path = "/tmp/" + program_name;

    // Prepare arguments for exec (program name + user args + NULL)
    char** exec_args = new char*[argc - 1];  // -1 because we skip "launch"
    exec_args[0] = strdup(container_path.c_str());
    for (int i = 3; i < argc; i++) {
        exec_args[i - 2] = argv[i];
    }
    exec_args[argc - 2] = nullptr;

    std::cout << "Launching: " << program << std::endl;

    // TODO: This is where namespace creation and sandbox setup will happen
    // For now, placeholder implementation
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork");
        delete[] exec_args;
        return 1;
    }
    
    if (pid == 0) {
        // Child process - will become the sandboxed program
        
        // Setup chroot (must happen before exec)
        if (setup_chroot() != 0) {
            std::cerr << "Failed to setup chroot\n";
            exit(1);
        }

        // TODO: Create namespaces with clone() instead of fork()
        // TODO: Mount /proc, /sys, /dev
        // TODO: Drop privileges if needed
        
        // For now, just exec the program (no isolation yet)
        execvp(exec_args[0], exec_args);
        
        // If exec fails
        perror("exec");
        exit(1);
    }
        
    // Parent process
    delete[] exec_args;

    std::cout << "Sandbox launched with PID: " << pid << std::endl;

    //FIXME: Add option to detach. Figure out the position of the argument and how to handle it properly. For now, we will just wait for the child to finish.
    // Parent process - optional: don't wait
    // if (argc > 3 && strcmp(argv[3], "--detach") == 0) {
    //     std::cout << "Sandbox launched in background with PID: " << pid << std::endl;
    //     return 0;  // Don't wait
    // }

    // Wait for child to complete
    int status;
    waitpid(pid, &status, 0);
    

    std::cout << "-----------------------------------------" << std::endl; //Empty line
    if (WIFEXITED(status)) {
        std::cout << "Sandbox exited with status: " << WEXITSTATUS(status) << std::endl;
    } else if (WIFSIGNALED(status)) {
        std::cout << "Sandbox terminated by signal: " << WTERMSIG(status) << std::endl;
    }
    

    // Cleanup: Remove the copied program from rootfs/tmp
    std::cout << "Cleaning up...\n";
    std::string cleanup_cmd = "rm -f " + target_path;
    system(cleanup_cmd.c_str());

    return 0;
}



int cmd_terminate(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: No PID specified\n";
        std::cerr << "Usage: terminate <pid>\n";
        return 1;
    }

    pid_t pid = std::stoi(argv[2]);
    
    std::cout << "Terminating sandbox with PID: " << pid << std::endl;
    
    // Send SIGTERM first (graceful)
    if (kill(pid, SIGTERM) < 0) {
        perror("kill");
        return 1;
    }
    
    // Give it a moment to terminate gracefully
    sleep(1);
    
    // Check if still running, send SIGKILL if needed
    if (kill(pid, 0) == 0) {
        std::cout << "Process still running, sending SIGKILL\n";
        kill(pid, SIGKILL);
    }
    
    std::cout << "Sandbox terminated\n";
    return 0;
}


int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string command = argv[1];

    if (command == "create") {
        return cmd_create();
    } 
    else if (command == "launch") {
        return cmd_launch(argc, argv);
    } 
    else if (command == "terminate") {
        return cmd_terminate(argc, argv);
    } 
    else {
        std::cerr << "Error: Unknown command '" << command << "'\n";
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}
