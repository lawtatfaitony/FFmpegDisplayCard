#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>



class ThreadPool
{
public:
    inline ThreadPool()
        : m_bStoped(false)
    {
    }

    inline ~ThreadPool()
    {
        Stop();
    }

public:
    void Start(unsigned short size = 1, int nMaxThread = 1)
    {
        m_nMaxThread = nMaxThread;
        add_thread(size);
    }

    void Stop()
    {
        m_bStoped.store(true);
        m_cvTask.notify_all(); // ���������߳�ִ��
        for (std::thread& thread : m_vecPool)
        {
            if (thread.joinable())
                thread.join(); // �ȴ���������� ǰ�᣺�߳�һ����ִ����
        }
    }

    // �ύһ������
    // ����.get()��ȡ����ֵ��ȴ�����ִ����,��ȡ����ֵ
    // �����ַ�������ʵ�ֵ������Ա��
    // һ����ʹ��   bind�� .commit(std::bind(&Dog::sayHello, &dog));
    // һ������ mem_fn�� .commit(std::mem_fn(&Dog::sayHello), &dog)
    template<class F, class... Args>
    auto Commit(F&& f, Args&&... args) ->std::future<decltype(f(args...))>
    {
        if (m_bStoped.load())    // stop == true ??
            throw std::runtime_error("commit on Threadm_vecPool is stopped.");

        using RetType = decltype(f(args...)); // typename std::result_of<F(Args...)>::type, ���� f �ķ���ֵ����
        auto task = std::make_shared<std::packaged_task<RetType()> >(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));    // wtf !
        std::future<RetType> future = task->get_future();
        {    // ������񵽶���
             //�Ե�ǰ���������  lock_guard �� mutex �� stack ��װ�࣬�����ʱ�� lock()��������ʱ�� unlock()
            std::lock_guard<std::mutex> lock{ m_lock };
            m_queTasks.emplace
            (
                [task]() {(*task)(); }
            );
        }
#ifdef THREAD_Pool_AUTO_GROW
        if (m_nThread < 1 && m_vecPool.size() < MAX_THREAD_NUM)
            addThread(1);
#endif // !THREADm_vecPool_AUTO_GROW
        m_cvTask.notify_one(); // ����һ���߳�ִ��

        return future;
    }

    //�����߳�����
    int GetAvailableThread() { return m_nThread; }
    //�߳�����
    int GetPoolSize() { return m_vecPool.size(); }

private:
    void add_thread(int size)
    {
        for (; m_vecPool.size() < m_nMaxThread && size > 0; --size)
        {   //��ʼ���߳�����
            m_vecPool.emplace_back([this] { // �����̺߳���
                while (!this->m_bStoped)
                {
                    std::function<void()> task;
                    {   // ��ȡһ����ִ�е� task
                        // unique_lock ��� lock_guard �ĺô��ǣ�������ʱ unlock() �� lock()
                        std::unique_lock<std::mutex> lock{ this->m_lock };
                        this->m_cvTask.wait(lock,
                            [this] {return this->m_bStoped.load() || !this->m_queTasks.empty(); }
                        ); // wait ֱ���� task
                        if (this->m_bStoped && this->m_queTasks.empty())return;
                        task = std::move(this->m_queTasks.front()); // ȡһ�� task
                        this->m_queTasks.pop();
                    }
                    --m_nThread;
                    task();
                    ++m_nThread;
                }
            });
            ++m_nThread;
        }
    }

private:
    using Task = std::function<void()>;
    // �̳߳�
    std::vector<std::thread> m_vecPool;
    // �������
    std::queue<Task> m_queTasks;
    // ͬ��
    std::mutex m_lock;
    // ��������
    std::condition_variable m_cvTask;
    // �Ƿ�ر��ύ
    std::atomic<bool> m_bStoped;
    //�����߳�����
    std::atomic<int> m_nThread;
    int m_nMaxThread;

};
