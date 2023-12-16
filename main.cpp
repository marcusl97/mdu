#include <iostream>
#include <stack>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <filesystem>
#include <unistd.h>

/*
 * Implementation of mdu, Measure Disk Usage
 *
 * Author: Marcus Lundqvist.
 *
 *
 * Version information:
 */

// Struct for the threads to read from and write to
typedef struct ThreadInfo
{
    std::vector<std::pair<std::stack<std::string>, size_t>> stack;
    int error{0};
    int process_count{0};
    bool no_more_work{false};
    std::mutex mutex;
    std::condition_variable work_available;
    std::condition_variable threads_complete;


} ThreadInfo;

/**
 * check_num_threads() - Checks for the -j flag and number of threads.
 * @argc: Argument count.
 * @argv: Argument values.
 *
 * Returns: Number of threads.
 *
 */
int check_num_threads(int argc, char *argv[]);

/**
 * add_directory() - loops trough directory.
 * @threadInfo: Struct containing information for the threads.
 * @path: Path to directory
 *
 * Returns: 0 if sucessfull, 1 if error occured.
 *
 */
int add_directory(ThreadInfo &threadInfo, const std::string& path);

/**
 * threadFunction() - Function run by every thread.
 * Adds folder to stack to process and processes folders.
 *
 * @arg: Used to pass variables.
 *
 * Returns: Nothing.
 *
 */
void thread_function(void *arg);

/**
 * init_threads() - Initiates all the threads.
 *
 * @num_threads: Number of threads to initiate.
 * @threadInfo: Struct containing information for the threads.
 *
 * Returns: Nothing.
 *
 */
void init_threads(int num_threads, int argc, char *argv[], ThreadInfo &threadInfo);





int main(int argc, char *argv[])
{
    // Init the threadInfo struct
    ThreadInfo threadInfo;
    // Get the number of threads to use
    int num_threads = check_num_threads(argc, argv);
    // Start threads
    init_threads(num_threads, argc, argv, threadInfo);
    // Check if an error occurred
    int exit_value = threadInfo.error;

    return exit_value;
}

void init_threads(int num_threads, int argc, char *argv[], ThreadInfo &threadInfo)
{
    namespace fs = std::filesystem;

    std::vector<std::thread> threads;

    // Create threads
    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back([&threadInfo]() { thread_function(&threadInfo); });
    }

    // Process directories
    for (int i = optind; i < argc; ++i)
    {
        fs::path path(argv[i]);
        if (fs::exists(path) && fs::is_directory(path))
        {
            std::stack<std::string> newStack;
            newStack.push(path.string());

            {
                std::lock_guard<std::mutex> lock(threadInfo.mutex);
                threadInfo.stack.emplace_back(newStack, 0);
                // Signal to threads that work is available
                threadInfo.work_available.notify_one();
            }

            // Wait for threads to complete
            {
                std::unique_lock<std::mutex> lock(threadInfo.mutex);
                threadInfo.threads_complete.wait(lock);
            }

            // Print result
            std::cout << threadInfo.stack.back().second << " " << path << '\n';
        }
        else
        {
            std::cout << fs::file_size(path) << " " << path << std::endl;
        }
    }

    // Signal that there is no more work to do
    {
        std::lock_guard<std::mutex> lock(threadInfo.mutex);
        threadInfo.no_more_work = true;
        threadInfo.work_available.notify_all();
    }

    // Wait for threads to finish
    for (std::thread &th : threads)
    {
        th.join();
    }
}
void thread_function(void *arg)
{
    auto &threadInfo = *static_cast<ThreadInfo *>(arg);

    while (true)
    {
        std::unique_lock<std::mutex> lock(threadInfo.mutex);

        // Check if stack is empty and no thread is working
        while (threadInfo.stack.empty() || (threadInfo.stack.back().first.empty() && (threadInfo.process_count > 0)))
        {
            //std::cout <<  "1 "<< std::endl;
            threadInfo.work_available.wait(lock);
        }

        if (threadInfo.stack.back().first.empty())
        {
            //std::cout <<  "2 "<< std::endl;
            threadInfo.threads_complete.notify_one();
            // Wait for work_available
            lock.unlock();
            threadInfo.work_available.wait(lock);

            // Check if there is no more work after waiting
            if (threadInfo.no_more_work)
            {
                // Exit if there is no more work
                //std::cout <<  "3 "<< std::endl;
                return;
            }
        }
        else
        {
            //std::cout <<  "4 "<< std::endl;
            std::string path = threadInfo.stack.back().first.top();
            threadInfo.stack.back().first.pop();
            threadInfo.process_count++;

            lock.unlock();

            int error = add_directory(threadInfo, path);

            lock.lock();

            if (error == 1)
            {
                threadInfo.error = error;
            }
            threadInfo.process_count--;

            // Wake up every thread to see if work is done
            threadInfo.work_available.notify_all();
        }
    }
}

int add_directory(ThreadInfo &threadInfo, const std::string &path)
{
    namespace fs = std::filesystem;
    int error = 0;
    size_t size = 0;

    for (const auto &entry : fs::directory_iterator(path))
    {
        if (entry.is_directory())
        {
            if (fs::exists(entry.path()) && (fs::status(entry.path()).permissions() & fs::perms::owner_read) != fs::perms::none)
            {
                {
                    std::unique_lock<std::mutex> lock(threadInfo.mutex);
                    threadInfo.stack.back().first.push(entry.path().string());
                    // Wake up one
                    threadInfo.work_available.notify_one();
                }
            }
            else
            {
                std::cerr << "Cannot read directory '" << entry.path() << "': Permission denied\n";
                size += fs::file_size(entry.path());
                // Set error variable
                error = 1;
            }
        }
        else
        {
            size += fs::file_size(entry.path());
        }
    }

    {
        std::unique_lock<std::mutex> lock(threadInfo.mutex);
        threadInfo.stack.at(0).second += size;
    }

    return error;
}

int check_num_threads(int argc, char *argv[])
{
    int num_threads = 1;

    int opt;
    while ((opt = getopt(argc, argv, "j:")) != -1)
    {
        switch (opt)
        {
            case 'j':
                num_threads = std::stoi(optarg);
                std::cout << "num_threads: " << num_threads << '\n';
                break;
        }
    }

    if (num_threads < 1)
    {
        std::cerr << "Error: Number of threads must be a positive number." << std::endl;
        exit(EXIT_FAILURE);
    }
    else
    {
        return num_threads;
    }
}