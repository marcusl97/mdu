#include <iostream>
#include <stack>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <filesystem>
#include <chrono>

/*
 * Implementation of mdu,
 * a program that uses multithreading to measure disk usage.
 * Usage: mdu -j {number of threads} {file} [files ...]
 *
 * Author: Marcus Lundqvist.
 *
 *
 * Version information:
 */

// Struct for the threads to read from and write to
typedef struct ThreadInfo
{
    std::pair<std::stack<std::string>, std::uintmax_t> stack;
    int error{0};
    int process_count{0};
    bool no_more_work{false};
    std::mutex mutex;
    std::condition_variable work_available;
    std::condition_variable threads_complete;


} ThreadInfo;

class Timer
{
private:
    // Type aliases to make accessing nested type easier
    using Clock = std::chrono::steady_clock;
    using Second = std::chrono::duration<double, std::ratio<1> >;

    std::chrono::time_point<Clock> m_beg { Clock::now() };

public:
    void reset()
    {
        m_beg = Clock::now();
    }

    double elapsed() const
    {
        return std::chrono::duration_cast<Second>(Clock::now() - m_beg).count();
    }
};

/**
 * check_num_threads() - Checks for the -j flag and number of threads.
 * @argc: Argument count.
 * @argv: Argument values.
 *
 * Returns: Number of threads to use and paths to measure.
 *
 */
std::pair<std::vector<std::string>, int> check_num_threads(int argc, char *argv[]);

/**
 * add_directory() - loops trough directory.
 * @threadInfo: Struct containing information for the threads.
 * @path: Path to directory
 *
 * Returns: 0 if successful, 1 if error occurred.
 *
 */
int add_directory(ThreadInfo &threadInfo, const std::string& path);

/**
 * threadFunction() - Function run by every thread.
 * Adds folder to stack to process and processes folders.
 *
 * @arg: Used to pass ThreadInfo struct.
 *
 * Returns: Nothing.
 *
 */
void thread_function(void *arg);

/**
 * init_threads() - Initiates all the threads.
 *
 * @cmdArgs: Contains files to measure and number of threads to init.
 * @threadInfo: Struct containing information for the threads.
 *
 * Returns: Nothing.
 *
 */
void init_threads(std::pair<std::vector<std::string>, int>& cmdArgs, ThreadInfo &threadInfo);

int main(int argc, char *argv[])
{
    // Init the threadInfo struct
    ThreadInfo threadInfo;

    // Get the number of threads to use
    std::pair<std::vector<std::string>, int> cmdArgs{check_num_threads(argc, argv)};
    //Start Timer
    Timer t;
    // Start threads
    init_threads(cmdArgs, threadInfo);
    // Print time
    std::cout << "Time elapsed: " << t.elapsed() << " seconds\n";
    // Check if an error occurred
    int exit_value = threadInfo.error;

    return exit_value;
}

void init_threads(std::pair<std::vector<std::string>, int>& cmdArgs, ThreadInfo &threadInfo)
{
    namespace fs = std::filesystem;

    std::vector<std::thread> threads;
    threads.reserve(cmdArgs.second); // Preallocate memory

    // Create threads
    for (int t = 0; t < cmdArgs.second; ++t)
    {
        threads.emplace_back([&threadInfo]() { thread_function(&threadInfo); });
    }

    // Process directories
    for (const auto& dir : cmdArgs.first)
    {
        fs::path path(dir);
        if (fs::exists(path) && fs::is_directory(path))
        {
            std::stack<std::string> newStack;
            newStack.push(path.string());

            {
                std::lock_guard<std::mutex> lock(threadInfo.mutex);
                threadInfo.stack.first = newStack;
                // Signal to threads that work is available
                threadInfo.work_available.notify_one();
            }

            // Wait for threads to complete
            {
                std::unique_lock<std::mutex> lock(threadInfo.mutex);
                threadInfo.threads_complete.wait(lock);
            }

            // Print result
            std::cout << "Path: " << path << " Size: " <<threadInfo.stack.second << '\n';
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
    // wait for work
    {
        std::unique_lock<std::mutex> lock(threadInfo.mutex);
        threadInfo.work_available.wait(lock);
    }

    while (true)
    {
        std::unique_lock<std::mutex> lock(threadInfo.mutex);

        // Check if stack is empty and no thread is working
        while (threadInfo.stack.first.empty() && (threadInfo.process_count > 0))
        {
            //std::cout << "1 " << std::endl;
            threadInfo.work_available.wait(lock);

        }

        if(threadInfo.no_more_work)
        {
            //std::cout <<  "2 "<< std::endl;
            lock.unlock();
            return;
        }

        if (threadInfo.stack.first.empty())
        {
            //std::cout <<  "3 "<< std::endl;
            threadInfo.threads_complete.notify_one();
            // Wait for work_available
            threadInfo.work_available.wait(lock);
            lock.unlock();
        }
        else
        {
            //std::cout <<  "5 "<< std::endl;
            std::string path = threadInfo.stack.first.top();
            threadInfo.stack.first.pop();
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
    std::uintmax_t size = 0;

    for (const auto &entry : fs::directory_iterator(path))
    {
        if (fs::is_symlink(entry))
        {
            // Do not add symbolic links to stack
            size += fs::file_size(entry.path());
            continue;
        }

        if (entry.is_directory())
        {
            if (fs::exists(entry.path()) && (fs::status(entry.path()).permissions() & fs::perms::owner_read) != fs::perms::none)
            {
                {
                    std::unique_lock<std::mutex> lock(threadInfo.mutex);
                    threadInfo.stack.first.push(entry.path().string());
                    // Wake up one
                    threadInfo.work_available.notify_one();
                }
            }
            else
            {
                std::cerr << "Cannot read directory '" << entry.path() << "': Permission denied\n";
                // Set error variable
                error = 1;
            }
        }
        else
        {
            size += fs::file_size(entry.path());
            //std::cout << "Path: " << entry.path() << " size: " << size << '\n';
        }
    }

    {
        std::unique_lock<std::mutex> lock(threadInfo.mutex);
        threadInfo.stack.second += size;
    }

    return error;
}

std::pair<std::vector<std::string>, int> check_num_threads(int argc, char *argv[])
{
    int numThreads = 1; // Default to 1 thread
    int maxThreads = static_cast<int>(std::thread::hardware_concurrency());
    std::vector<std::string> files;

    // Check if the -j flag is provided and parse the number of threads and files
    for (int i = 1; i < argc; i++)
    {
        try
        {
            if (std::string(argv[i]) == "-j")
            {
                std::cout << "Num threads: " << argv[i + 1] << '\n';
                numThreads = std::stoi(argv[++i]);
                // Check if numThreads is greater than hardware concurrency
                if (numThreads > maxThreads)
                {
                    std::cerr << "Number of threads exceeds hardware concurrency. Maximum supported threads: "
                              << maxThreads << '\n';
                    numThreads = maxThreads;
                }

            }
            else
            {
                files.emplace_back(argv[i]);
                std::cout << "File: " << argv[i] << '\n';
            }
        }
        catch (const std::exception& e)
        {
            // Handle the exception (e.g., print an error message)
            std::cerr << "Error: " << e.what() << " ,Usage: mdu -j {number of threads} {file} [files ...] " << '\n';
            exit(EXIT_FAILURE);
        }


    }

    std::cout << "Number of threads: " << numThreads << '\n';

    // Store the values in cmdArgs
    std::pair<std::vector<std::string>, int> cmdArgs = {files, numThreads};

    return cmdArgs;
}