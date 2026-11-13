// 첫 화면이 깜빡이지 않도록, 저장된 테마/스킨을 렌더링 전에 적용한다. (index.html 의 <head> 에서 불러온다)
try {
  var d = document.documentElement, t = localStorage.getItem('lunaris-theme'), s = localStorage.getItem('lunaris-skin');
  if (t) d.dataset.theme = t;
  if (s) d.dataset.skin = s;
} catch (e) {}
