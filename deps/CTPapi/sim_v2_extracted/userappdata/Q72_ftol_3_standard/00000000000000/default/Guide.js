// JavaScript Document

function init()
{
	for (var j=0;j<document.all.length;j++)
	{
		if (document.all[j].className=="answer")
		{
			var targ = document.all[j];
			if (targ.childNodes[0].tagName =="INPUT")
			{
				if (targ.childNodes[0].type =="checkbox")
				{
					document.all[j].attachEvent("onmouseover", function(){checkshow(event);});
					document.all[j].attachEvent("onclick", function(){selectitem(event);});
				}else{
					document.all[j].attachEvent("onclick", function(){checkshow(event);});
					document.all[j].attachEvent("onclick", function(){selectitem(event);});
				}
			}
		}
	}
	setTimeout("initradio()",100);
}
function initradio()
{
	var b = false;
	for (var j=0;j<document.all.length;j++)
	{
		if (document.all[j].tagName=="INPUT" && document.all[j].type=="radio")
		{
			if (document.all[j].checked)
			{
				checkshow(document.all[j]);
				return;
			}else{
				b = true;
			}
		}
	}
	if (b)
	{
		setTimeout("initradio()",100);
	}
}
function selectitem(e)
{
	var targ;
	if (e.target)
	{
		targ=e.target;
	}
	else if (e.srcElement)
	{
		targ=e.srcElement;
	}else{
		targ = e;
	}
	if (targ.tagName=="INPUT")
	{
		if (targ.type == "checkbox")
		{
			return
		}
		targ = targ.parentNode;
	}
	while(targ.className!="answer" && targ.className!="selectedanswer")
	{
		targ = targ.parentNode;
	}	
	//寻找对应INPUT
	selecteditem = targ.childNodes[0]
	//打开选中项的显示
	if (selecteditem.type == "checkbox")
	{
		selecteditem.checked = !selecteditem.checked;
	}else{
		selecteditem.checked = true;
	}
}
function checkshow(e)
{
	try{
		var targ;
		if (e.target)
		{
			targ=e.target;
		}
		else if (e.srcElement)
		{
			targ=e.srcElement;
		}else{
			targ = e;
		}
		if (targ.tagName=="INPUT")
		{
			targ = targ.parentNode;
		}
		//隐藏所有箭头
		for (var j=0;j<document.Answers.elements.length;j++)
		{
			document.Answers.elements[j].parentNode.className = "answer";
		}
		//隐藏所有提示
		for (var i=0; i<10;i++)
		{
			var id = ""+i;
			if (document.getElementById(id))
			{
				document.getElementById(id).style.display = "none";
			}
		}
		//寻找当前包含项
		while(targ.className!="answer" && targ.className!="selectedanswer")
		{
			targ = targ.parentNode;
		}		
		targ.className = "selectedanswer";
		//寻找对应INPUT
		selecteditem = targ.childNodes[0]
		//打开选中项的显示
		document.getElementById(selecteditem.value).style.display = "block";
	}catch(e)
	{
	}
}