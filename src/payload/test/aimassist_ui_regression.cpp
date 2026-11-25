#include "core/setting/KeyCaptureState.h"
#include "core/setting/NumericPrecision.h"
#include <iostream>
#include <stdexcept>
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
 try {
  dt::KeyCaptureState capture;dt::KeyCaptureState::Snapshot keys{};
  keys[1]=keys[16]=true;capture.start(keys);
  require(!capture.poll(keys,161),"Opening click or held modifier was captured");
  keys[16]=false;require(!capture.poll(keys,161),"Key release was captured");
  keys['R']=true;auto letter=capture.poll(keys,161);require(letter&&letter->virtualKey=='R'&&!letter->cancelled,"Fresh focus key not captured");
  require(!capture.active(),"Successful capture stayed armed");
  keys={};capture.start(keys);keys[27]=true;auto cancel=capture.poll(keys,161);require(cancel&&cancel->cancelled&&!capture.active(),"Escape does not cancel without replacement");
  keys={};capture.start(keys);keys[46]=true;auto clear=capture.poll(keys,161);require(clear&&!clear->cancelled&&clear->virtualKey==0,"Delete does not clear bind");
  keys={};capture.start(keys);keys[8]=true;clear=capture.poll(keys,161);require(clear&&clear->virtualKey==0,"Backspace does not clear bind");
  keys={};capture.start(keys);keys[161]=true;require(!capture.poll(keys,161),"Native menu key was bound as Focus");
  keys[5]=true;auto side=capture.poll(keys,161);require(side&&side->virtualKey==5,"Side mouse button is not accepted");
  keys={};capture.start(keys);capture.cancel();keys['G']=true;require(!capture.poll(keys,161),"Cancelled capture consumes a later game key");
  require(dt::numericDecimalPlaces(.001f)==3,"Prediction precision is lost");
  require(dt::numericDecimalPlaces(.01f)==2,"Float representation adds a spurious decimal");
  require(dt::numericDecimalPlaces(.1f)==1,"Tenths precision is incorrect");
  require(dt::numericDecimalPlaces(1)==0,"Integer step should display integer precision");
  require(dt::numericDecimalPlaces(.05f)==2,"Half-tenth step precision is incorrect");
  require(dt::numericDecimalPlaces(0)==1,"Invalid numeric step has no fallback");
  std::cout<<"PASS: auxiliary key capture, cancellation and prediction precision\n";return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
