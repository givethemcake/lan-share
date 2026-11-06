#pragma once
#include"iostream"
#include <string>
using namespace std;

//NOT thread safe on its own whoever uses it from multiple threads has to lock a mutex around it

template <typename T>
class Node {
public:
    T data;
    Node<T>* next;
    Node(T new_data) {
        this->data = new_data;
        this->next = nullptr;
    }
};

template <typename T>
class Queue {
    Node<T>* front, * rear;
public:

        Queue() { front = rear = nullptr; }
        ~Queue() { clear(); }

        //owns raw pointers so copying would double delete just dont allow it
        Queue(const Queue&) = delete;
        Queue& operator=(const Queue&) = delete;

        bool is_empty() { return front == nullptr; }

        bool enqueue(T new_data) {

            Node<T>* new_node = new Node<T>(new_data);
            if (is_empty()) {
                // If queue is empty the new node is both front and rear
                front = rear = new_node;
                return true;
            }
            // Add the new node at the end of the queue and move rear
            rear->next = new_node;
            rear = new_node;
            return true;
        }

        T dequeue() {
            if (is_empty()) return T{}; //NULL only works for pointer/number T T{} works for structs too

            // Store previous front and move front one node ahead
            Node<T>* old_front = front;
            T data = old_front->data;
            front = front->next;
            // If front becomes nullptr then change rear also to nullptr
            if (front == nullptr) rear = nullptr;
            // Deallocate memory of the old front node
            delete old_front;
            return data;
        }

        T peek()
        {
            if (front == nullptr) return T{};
            return front->data;
        }


        int size()
        {
            Node<T>* ptr = front;
            int size = 0;
            while (ptr != nullptr)
            {
                size++;
                ptr = ptr->next;
            }
            return size;
        }

        void clear()
        {
            Node<T>* ptr = front;

            while (ptr != nullptr) //old version never deleted the last node
            {
                Node<T>* next = ptr->next;
                delete ptr;
                ptr = next;
            }

            front = rear = nullptr;
            return;
        }

        bool find(T val)
        {
            Node<T>* ptr = front;

            while (ptr != nullptr)
            {
                if (ptr->data == val) return true;
                ptr = ptr->next;
            }

            return false;
        }

};
