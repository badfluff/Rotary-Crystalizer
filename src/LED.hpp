#pragma once

#include <Arduino.h>

class LED{
    private:
        int ID;
        bool state;
        unsigned long prevMillis;

    public:
        LED(const int& ID_in) : ID(ID_in), state(false), prevMillis(0) {pinMode(ID, OUTPUT);}
        
        //RETURNS:true when LED is on, false when LED is off
        bool getState() {return state;}

        //Sets LED pin to HIGH
        void high(){
            state = true;
            digitalWrite(ID, HIGH);
        }

        //Sets LED pin to LOW
        void low(){
            state = false;
            digitalWrite(ID, LOW);
        }

        //Toggle LED state
        void toggle(){
            state = !state;
            digitalWrite(ID, state ? HIGH : LOW);
        }

        void flash(const int interval){
            unsigned long currentMillis = millis();

            if (currentMillis - prevMillis >= interval) {
               // Serial.println(currentMillis - prevMillis);
                prevMillis = currentMillis;
                state = !state;
                digitalWrite(ID, state ? HIGH : LOW);
            }
        }        

};